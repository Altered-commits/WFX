cmake_minimum_required(VERSION 3.15)
project(wfx)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------
# Include third-party dependencies
# -----------------------------
include(cmake/third_party.cmake)

# -----------------------------
# Output directories
# -----------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

# -----------------------------
# Gather sources
# -----------------------------
file(GLOB_RECURSE WFX_CLI_SOURCES       "${CMAKE_CURRENT_SOURCE_DIR}/cli/*.cpp")
file(GLOB_RECURSE WFX_CONFIG_SOURCES    "${CMAKE_CURRENT_SOURCE_DIR}/config/*.cpp")
file(GLOB_RECURSE WFX_ENGINE_SOURCES    "${CMAKE_CURRENT_SOURCE_DIR}/engine/*.cpp")
file(GLOB_RECURSE WFX_HTTP_SOURCES      "${CMAKE_CURRENT_SOURCE_DIR}/http/*.cpp")
file(GLOB_RECURSE WFX_UTILS_SOURCES     "${CMAKE_CURRENT_SOURCE_DIR}/utils/*.cpp")
file(GLOB_RECURSE WFX_SHARED_SOURCES    "${CMAKE_CURRENT_SOURCE_DIR}/shared/*.cpp")

if(WIN32)
    file(GLOB_RECURSE WFX_OS_SPECIFIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/os_specific/windows/*.cpp")
elseif(UNIX)
    file(GLOB_RECURSE WFX_OS_SPECIFIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/os_specific/linux/*.cpp")
endif()

# -----------------------------
# Engine executable target
# -----------------------------
add_executable(wfx
    ${WFX_CLI_SOURCES}
    ${WFX_CONFIG_SOURCES}
    ${WFX_ENGINE_SOURCES}
    ${WFX_HTTP_SOURCES}
    ${WFX_UTILS_SOURCES}
    ${WFX_SHARED_SOURCES}
    ${WFX_OS_SPECIFIC_SOURCES}
)

target_include_directories(wfx PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
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
    target_link_libraries(wfx PRIVATE
        dl
        uring
    )
endif()

# Link third-party libraries
target_link_libraries(wfx PRIVATE
    tlsf
    concurrentqueue
    nlohmann_json
    tomlplusplus
)

# -----------------------------
# Separate utils shared library target
# -----------------------------
file(GLOB_RECURSE WFX_UTILS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/utils/*.cpp")

if(WIN32)
    file(GLOB_RECURSE WFX_OS_UTILS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/os_specific/windows/utils/*.cpp")
elseif(UNIX)
    file(GLOB_RECURSE WFX_OS_UTILS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/os_specific/linux/utils/*.cpp")
endif()

# Merge common and OS-specific utils
list(APPEND WFX_UTILS_SOURCES ${WFX_OS_UTILS_SOURCES})

# Create the wfxutils library
add_library(wfxutils SHARED ${WFX_UTILS_SOURCES})

target_include_directories(wfxutils PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

# OS-specific system libraries
if(WIN32)
    target_link_libraries(wfxutils PRIVATE bcrypt)
elseif(UNIX)
    target_link_libraries(wfxutils PRIVATE dl)
endif()

# Link third-party libraries
target_link_libraries(wfxutils PRIVATE
    tlsf
    concurrentqueue
    nlohmann_json
    tomlplusplus
)

set_target_properties(wfxutils PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib
)

# -----------------------------
# Create a custom target to build utils only
# -----------------------------
add_custom_target(build_utils
    DEPENDS wfxutils
    COMMENT "Building utils shared library only"
)