cmake_minimum_required(VERSION 3.24)
project(wfx)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------
# Default to Release when no build type is specified
# -----------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Choose the type of build.")
endif()

# -----------------------------
# Set actively used OS
# -----------------------------
if(WIN32)
    set(CURRENT_OS windows)
elseif(APPLE)
    set(CURRENT_OS macos)
elseif(UNIX)
    set(CURRENT_OS linux)
else()
    message(FATAL_ERROR "Unsupported OS")
endif()

# -----------------------------
# Include third-party dependencies
# -----------------------------
include(cmake/third_party.cmake)

# -----------------------------
# Output directories
# -----------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

# -----------------------------
# Gather sources (exclude utils here)
# -----------------------------
# But before we do it, add this helper function to root out specific OS folders
function(filter_sources_by_os out_var)
    set(filtered_sources "")
    foreach(src IN LISTS ${out_var})
        if(NOT src MATCHES "/(windows|linux|macos)/")
            list(APPEND filtered_sources "${src}")
        elseif(src MATCHES "/${CURRENT_OS}/")
            list(APPEND filtered_sources "${src}")
        endif()
    endforeach()
    set(${out_var} "${filtered_sources}" PARENT_SCOPE)
endfunction()

if(CMAKE_BUILD_TYPE MATCHES "Debug")
    add_compile_definitions(
        $<$<CONFIG:Debug>:WFX_DEBUG>
    )
endif()

file(GLOB_RECURSE WFX_CLI_SOURCES         "${CMAKE_CURRENT_SOURCE_DIR}/cli/*.cpp")
file(GLOB_RECURSE WFX_CONFIG_SOURCES      "${CMAKE_CURRENT_SOURCE_DIR}/config/*.cpp")
file(GLOB_RECURSE WFX_ENGINE_SOURCES      "${CMAKE_CURRENT_SOURCE_DIR}/engine/*.cpp")
file(GLOB_RECURSE WFX_HTTP_SOURCES        "${CMAKE_CURRENT_SOURCE_DIR}/http/*.cpp")
file(GLOB_RECURSE WFX_SHARED_SOURCES      "${CMAKE_CURRENT_SOURCE_DIR}/shared/*.cpp")
file(GLOB_RECURSE WFX_OS_SPECIFIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/os_specific/*.cpp")

filter_sources_by_os(WFX_OS_SPECIFIC_SOURCES)

# -----------------------------
# Engine executable target
# -----------------------------
add_executable(wfx
    ${WFX_CLI_SOURCES}
    ${WFX_CONFIG_SOURCES}
    ${WFX_ENGINE_SOURCES}
    ${WFX_HTTP_SOURCES}
    ${WFX_SHARED_SOURCES}
    ${WFX_OS_SPECIFIC_SOURCES}
)

target_include_directories(wfx PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

# Include ssl dependencies
include(cmake/ssl.cmake)

# Compile/link time optimizations
target_compile_options(wfx PRIVATE
    # Debug
    $<$<CONFIG:Debug>:-g -O0>

    # Release (GCC)
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU>>:
        -O3 -ffunction-sections -fdata-sections -fvisibility=hidden -flto=auto -fmerge-all-constants
    >

    # Release (Clang / AppleClang)
    $<$<AND:$<CONFIG:Release>,$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>>:
        -O3 -ffunction-sections -fdata-sections -fvisibility=hidden -flto=auto -fmerge-all-constants
    >

    # Release (Intel oneAPI / icx)
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:IntelLLVM>>:
        -O3 -ffunction-sections -fdata-sections -fvisibility=hidden -flto=auto -fmerge-all-constants -xHost
    >

    # Release (MSVC)
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:
        /O2 /GL /Gw /Gy /Zc:inline /GS-
    >
)

target_link_options(wfx PRIVATE
    # Release (GCC / Clang / AppleClang / Intel)
    $<$<AND:$<CONFIG:Release>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:IntelLLVM>>>:
        -Wl,--gc-sections -Wl,--as-needed -Wl,-s -flto=auto
    >

    # Release (MSVC)
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:
        /LTCG /OPT:REF /OPT:ICF
    >
)

# OS-specific system libraries
if(WIN32)
    target_link_libraries(wfx PRIVATE
        Ws2_32
        Dbghelp
        mswsock
        bcrypt
    )
elseif(UNIX)
    target_link_libraries(wfx PRIVATE dl pthread)

    if(WFX_LINUX_USE_IO_URING)
        target_link_libraries(wfx PRIVATE uring)
    endif()
endif()

# Link third-party libraries
target_link_libraries(wfx PRIVATE
    tlsf
    tomlplusplus
)

# -----------------------------
# Utils shared library target
# -----------------------------
file(GLOB_RECURSE WFX_UTILS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/utils/*.cpp")
filter_sources_by_os(WFX_UTILS_SOURCES)

add_library(wfxutils SHARED ${WFX_UTILS_SOURCES})

target_include_directories(wfxutils PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

if(WIN32)
    target_link_libraries(wfxutils PRIVATE bcrypt)
elseif(UNIX)
    target_link_libraries(wfxutils PRIVATE dl)
endif()

target_link_libraries(wfxutils PRIVATE
    tlsf
)

set_target_properties(wfxutils PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib
)

# -----------------------------
# Engine links against utils
# -----------------------------
target_link_libraries(wfx PRIVATE wfxutils)

# -----------------
# | Build summary |
# -----------------
message(STATUS "==================== WFX Build Summary ====================")
message(STATUS "Targets available:")
message(STATUS "  - wfx")
message(STATUS "  - wfxutils")
message(STATUS "Build type           : ${CMAKE_BUILD_TYPE}")
message(STATUS "Generator            : ${CMAKE_GENERATOR}")
message(STATUS "Compiler             : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "C++ standard         : Cxx${CMAKE_CXX_STANDARD}")
message(STATUS "Output directory     : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "===========================================================")
