cmake_minimum_required(VERSION 3.15)
project(wfx)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set the output directory for the executable to be in root
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

# Add CLI .cpp files
file(GLOB_RECURSE WFX_CLI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cli/*.cpp")
file(GLOB_RECURSE WFX_CONFIG_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/config/*.cpp")
file(GLOB_RECURSE WFX_ENGINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/engine/*.cpp")
file(GLOB_RECURSE WFX_HTTP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/http/*.cpp")
file(GLOB_RECURSE WFX_OS_SPECIFIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/os_specific/*.cpp")
file(GLOB_RECURSE WFX_THIRD_PARTY_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/*.cpp"
)
file(GLOB_RECURSE WFX_UTILS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/utils/*.cpp")
file(GLOB_RECURSE WFX_SHARED_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/shared/*.cpp")

# Add executable target
add_executable(wfx ${WFX_CLI_SOURCES} ${WFX_CONFIG_SOURCES} ${WFX_ENGINE_SOURCES} ${WFX_HTTP_SOURCES}
                    ${WFX_OS_SPECIFIC_SOURCES} ${WFX_THIRD_PARTY_SOURCES} ${WFX_UTILS_SOURCES} ${WFX_SHARED_SOURCES})

# Include directories for WFX
target_include_directories(wfx PRIVATE ${CMAKE_SOURCE_DIR})

# Link required libraries
target_link_libraries(wfx PRIVATE Ws2_32 mswsock bcrypt)

# Enable PDB generation in Release if PERF_PROF is defined
if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "Release")
    if(PERF_PROF)
        target_compile_options(wfx PRIVATE /Zi /Zo)
        target_link_options(wfx PRIVATE /DEBUG)
    endif()
endif()