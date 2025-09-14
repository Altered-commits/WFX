cmake_minimum_required(VERSION 3.15)
project(wfx)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Third party dependencies
include(cmake/third_party.cmake)

# Set the output directory for the executable to be in root
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

# Gather sources by module
file(GLOB_RECURSE WFX_CLI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cli/*.cpp")
file(GLOB_RECURSE WFX_CONFIG_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/config/*.cpp")
file(GLOB_RECURSE WFX_ENGINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/engine/*.cpp")
file(GLOB_RECURSE WFX_HTTP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/http/*.cpp")
file(GLOB_RECURSE WFX_UTILS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/utils/*.cpp")
file(GLOB_RECURSE WFX_SHARED_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/shared/*.cpp")

# OS-specific sources
if(WIN32)
    file(GLOB_RECURSE WFX_OS_SPECIFIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/os_specific/windows/*.cpp")
elseif(UNIX)
    file(GLOB_RECURSE WFX_OS_SPECIFIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/os_specific/linux/*.cpp")
endif()

# Add executable target
add_executable(wfx
    ${WFX_CLI_SOURCES}
    ${WFX_CONFIG_SOURCES}
    ${WFX_ENGINE_SOURCES}
    ${WFX_HTTP_SOURCES}
    ${WFX_UTILS_SOURCES}
    ${WFX_SHARED_SOURCES}
    ${WFX_OS_SPECIFIC_SOURCES}
)

# Include directories
target_include_directories(wfx PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

# Link OS-specific system libraries
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