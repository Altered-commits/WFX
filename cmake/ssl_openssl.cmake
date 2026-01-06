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

ExternalProject_Add(openssl_lts_build
    # Using 3.5.4 because it has LTS
    URL "https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz"
    URL_HASH SHA256=967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99
    DOWNLOAD_EXTRACT_TIMESTAMP true

    # Because Ninja is more stricter than make, because these files exist after we run the build
    # But Ninja aint gon care about all those stuff, IT NEEDS THEM
    BUILD_BYPRODUCTS 
        "${OPENSSL_INSTALL_DIR}/lib/libssl.so"
        "${OPENSSL_INSTALL_DIR}/lib/libcrypto.so"
    
    # We use cmake -E env to pass optimization flags to OpenSSL's non-CMake build system.
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env "CFLAGS=${OPENSSL_OPT_FLAGS}"
        <SOURCE_DIR>/Configure
            # Target platform / core features
            ${OPENSSL_TARGET}     # Explicitly set the target architecture
            enable-ktls           # Enable Kernel TLS offloading
            enable-asm            # Enable hand-optimized assembly routines for performance
            enable-ec_nistp_64_gcc_128 # Enable specific optimizations for NIST P-curves

            # Disable legacy features
            no-ssl3               # Disable obsolete SSL protocols
            no-weak-ssl-ciphers   # Disable EXPORT, LOW, and other weak ciphers
            no-comp               # Disable SSL/TLS compression (CRIME attack vector)
            no-zlib               # Disable zlib compression support
            no-dtls1              # Disable DTLSv1 (we are a TCP server)
            no-deprecated         # Remove support for deprecated APIs

            # Strip unused algos
            no-async no-aria no-camellia no-idea no-md2 no-md4 no-rc2 no-rc5
            no-whirlpool no-sctp no-gost

            # Build and installation options
            shared                # Make shared libs (.so)
            no-legacy             # Remove old legacy APIs
            no-tests              # Don't build the OpenSSL test suite
            --prefix=<INSTALL_DIR>
            --openssldir=<INSTALL_DIR>
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
add_library(OpenSSL::Crypto SHARED IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_INSTALL_DIR}/lib/libcrypto.so"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INSTALL_DIR}/include"
)

# SSL
add_library(OpenSSL::SSL SHARED IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_INSTALL_DIR}/lib/libssl.so"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INSTALL_DIR}/include"
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
)

# Ensure everything waits for the build
add_dependencies(OpenSSL::SSL openssl_lts_build)
add_dependencies(OpenSSL::Crypto openssl_lts_build)

message(STATUS "OpenSSL: Custom Linux build target configured successfully")