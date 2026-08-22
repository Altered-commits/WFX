cmake_minimum_required(VERSION 3.24)

project(user_plugin)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output directory
set(OUTPUT_DIR "${CMAKE_BINARY_DIR}")

# --------------------------------------------------
# Compile configuration
# --------------------------------------------------
function(configure_compile target)
    if(WIN32)
        set(WFX_HOME "$ENV{USERPROFILE}/.wfx/src")
    else()
        set(WFX_HOME "$ENV{HOME}/.wfx/src")
    endif()

    target_include_directories(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${WFX_HOME}/include"
        "${WFX_HOME}"
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /EHsc /Gw /Gy
            $<$<CONFIG:Release>:/O2 /GL /MD>
            $<$<CONFIG:Debug>:/Od /MDd>
        )
    else()
        target_compile_options(${target} PRIVATE
            -fPIC
            -Wno-return-type-c-linkage
            $<$<CONFIG:Release>:
                -O3
                -fvisibility=hidden
                -fvisibility-inlines-hidden
                -ffunction-sections
                -fdata-sections
            >
            $<$<CONFIG:Debug>:-O0>
        )

        set_target_properties(${target} PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES
        )
    endif()
endfunction()

# --------------------------------------------------
# Shared-library link configuration
# --------------------------------------------------
function(configure_shared target)
    if(MSVC)
        target_link_options(${target} PRIVATE
            /DLL
            $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /DEBUG:OFF>
            $<$<CONFIG:Debug>:/DEBUG>
        )

        set_target_properties(${target} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
        )
    else()
        if(APPLE)
            target_link_options(${target} PRIVATE
                $<$<CONFIG:Release>:-Wl,-dead_strip>
            )
        else()
            target_link_options(${target} PRIVATE
                $<$<CONFIG:Release>:
                    -Wl,--gc-sections
                    -Wl,--strip-all
                >
            )
        endif()

        set_target_properties(${target} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
        )
    endif()

    set_target_properties(${target} PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}"
    )
endfunction()

# vvv USER ENTRY (always defined) vvv
file(GLOB_RECURSE USER_ENTRY_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)

add_library(user_entry SHARED ${USER_ENTRY_SOURCES})
configure_compile(user_entry)
configure_shared(user_entry)

# vvv USER TEMPLATES (Engine guarantees directory + source exists) vvv
file(GLOB_RECURSE USER_TEMPLATE_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/intermediate/dynamic/*.cpp"
)

add_library(user_templates SHARED ${USER_TEMPLATE_SOURCES})
configure_compile(user_templates)
configure_shared(user_templates)

# -----------
# | Summary |
# -----------
message(STATUS "================ Build Configuration ================")
message(STATUS "Targets available:")
message(STATUS "  - user_entry")
message(STATUS "  - user_templates")
message(STATUS "Generator            : ${CMAKE_GENERATOR}")
message(STATUS "Compiler             : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "C++ standard         : Cxx${CMAKE_CXX_STANDARD}")
message(STATUS "Output directory     : ${OUTPUT_DIR}")
message(STATUS "===========================================================")
