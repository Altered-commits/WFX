include(ExternalProject)

# -------------------- NOT LINUX --------------------
if(WIN32 OR APPLE)
    message(STATUS "Windows/macOS detected. Looking for a pre-installed OpenSSL")
    message(STATUS "Please ensure OpenSSL is installed and available in your system's PATH")
    
    # Use CMakes standard find module to locate a pre installed OpenSSL
    find_package(OpenSSL REQUIRED)

    return()
endif()

# -------------------- LINUX --------------------
message(STATUS "OpenSSL: Linux detected. Configuring custom OpenSSL build")

# Detect the system CA directory. --openssldir must point here so that
# SSL_CTX_set_default_verify_paths() finds the system trust store at runtime.
# Each major distro family keeps its CA bundle in a different location:
#   /etc/ssl          -> Debian, Ubuntu, Arch, Alpine, Gentoo
#   /etc/pki/tls      -> RHEL, Fedora, CentOS, Rocky, AlmaLinux
#   /etc/ssl/openssl  -> some minimal/embedded distros
if(EXISTS "/etc/pki/tls")
    set(OPENSSL_SYSDIR "/etc/pki/tls")
elseif(EXISTS "/etc/ssl")
    set(OPENSSL_SYSDIR "/etc/ssl")
else()
    message(WARNING "OpenSSL: Could not detect system CA directory. Defaulting to /etc/ssl. "
                    "CA verification for outbound TLS connections may fail. "
                    "Set OPENSSL_SYSDIR manually if needed.")
    set(OPENSSL_SYSDIR "/etc/ssl")
endif()

message(STATUS "OpenSSL: Using system CA directory: ${OPENSSL_SYSDIR}")

# Get the running OS
string(TOLOWER "${CMAKE_SYSTEM_NAME}" OPENSSL_OS)

# Get the CPU architecture
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" OPENSSL_ARCH)

set(OPENSSL_TARGET "${OPENSSL_OS}-${OPENSSL_ARCH}")

# Get the number of cores we can use for parallelizing build
include(ProcessorCount)
ProcessorCount(NPROC)

if(NPROC LESS_EQUAL 4)
    set(NPROC 1)
elseif(NPROC GREATER 16)
    set(NPROC 16)
endif()

# Find the actual 'make' program (ignore Ninja)
find_program(MAKE_EXE NAMES make gmake REQUIRED)

# Set a directory within the build folder for the installation artifacts
set(OPENSSL_INSTALL_DIR ${CMAKE_BINARY_DIR}/openssl_lts-install)

# Prepare compiler flags for optimization
set(OPENSSL_OPT_FLAGS "-O3 -DOPENSSL_SMALL_FOOTPRINT")

# Static linking is for release binaries, which need to run standalone on a
# box that doesn't have this custom OpenSSL build's .so files anywhere on its
# linker path. Local/dev builds stay dynamic since nothing ships them off-box.
option(WFX_STATIC_SSL "Statically link the custom-built OpenSSL into wfx" OFF)

if(WFX_STATIC_SSL)
    set(OPENSSL_SHARED_OPT "no-shared")
    set(OPENSSL_LIB_TYPE STATIC)
    set(OPENSSL_LIB_EXT ".a")
else()
    set(OPENSSL_SHARED_OPT "shared")
    set(OPENSSL_LIB_TYPE SHARED)
    set(OPENSSL_LIB_EXT ".so")
endif()

ExternalProject_Add(openssl_lts_build
    # Using 3.5.4 because it has LTS support until April 8, 2030
    URL "https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz"
    URL_HASH SHA256=967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99
    DOWNLOAD_EXTRACT_TIMESTAMP true

    # Because Ninja is more stricter than make, because these files exist after we run the build
    # But Ninja aint gon care about all those stuff, IT NEEDS THEM
    BUILD_BYPRODUCTS
        "${OPENSSL_INSTALL_DIR}/lib/libssl${OPENSSL_LIB_EXT}"
        "${OPENSSL_INSTALL_DIR}/lib/libcrypto${OPENSSL_LIB_EXT}"

    # We use cmake -E env to pass optimization flags to OpenSSL's non-CMake build system.
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env "CFLAGS=${OPENSSL_OPT_FLAGS}"
        <SOURCE_DIR>/Configure
            # Target platform / core features
            ${OPENSSL_TARGET}              # Explicitly set the target architecture
            enable-ktls                    # Enable Kernel TLS offloading
            enable-asm                     # Enable hand-optimized assembly routines for performance
            enable-ec_nistp_64_gcc_128     # Enable specific optimizations for NIST P-curves

            # Disable legacy features
            no-ssl3                        # Disable obsolete SSL protocols
            no-weak-ssl-ciphers            # Disable EXPORT, LOW, and other weak ciphers
            no-comp                        # Disable SSL/TLS compression (CRIME attack vector)
            no-zlib                        # Disable zlib compression support
            no-dtls1                       # Disable DTLSv1 (we are a TCP server)
            no-deprecated                  # Remove support for deprecated APIs

            # Strip unused algos
            no-async no-aria no-camellia no-idea no-md2 no-md4 no-rc2 no-rc5
            no-whirlpool no-sctp no-gost

            # Build and installation options
            ${OPENSSL_SHARED_OPT}          # shared (.so) for dev builds, no-shared (.a only) for releases
            no-legacy                      # Remove old legacy APIs
            no-tests                       # Don't build the OpenSSL test suite
            --prefix=<INSTALL_DIR>
            --openssldir=${OPENSSL_SYSDIR} # Point at system CA dir so default verify paths work
            --libdir=lib

    # NOTE: Use ${MAKE_EXE} instead of ${CMAKE_MAKE_PROGRAM} to prevent Ninja related errors
    # Pass the parallel job count to the sub-make
    BUILD_COMMAND ${MAKE_EXE} -j${NPROC} build_libs

    # Also pass it to the install command
    INSTALL_COMMAND ${MAKE_EXE} -j${NPROC} install_dev
    
    # Final installation directory
    INSTALL_DIR ${OPENSSL_INSTALL_DIR}
)

# Just make it so cmake doesn't give error
file(MAKE_DIRECTORY ${OPENSSL_INSTALL_DIR}/include)

# Crypto
add_library(OpenSSL::Crypto ${OPENSSL_LIB_TYPE} IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_INSTALL_DIR}/lib/libcrypto${OPENSSL_LIB_EXT}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INSTALL_DIR}/include"
)

# SSL
add_library(OpenSSL::SSL ${OPENSSL_LIB_TYPE} IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_INSTALL_DIR}/lib/libssl${OPENSSL_LIB_EXT}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INSTALL_DIR}/include"
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
)

# Ensure everything waits for the build
add_dependencies(OpenSSL::SSL openssl_lts_build)
add_dependencies(OpenSSL::Crypto openssl_lts_build)

message(STATUS "OpenSSL: Custom Linux build target configured successfully")