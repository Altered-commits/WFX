include(FetchContent)

# Helper function for logging
function(fetch_and_log NAME)
    message(STATUS "Fetching and configuring ${NAME}...")
endfunction()

# ---------------- TLSF ----------------
fetch_and_log("TLSF")
FetchContent_Declare(
  tlsf
  GIT_REPOSITORY https://github.com/mattconte/tlsf.git
  GIT_TAG        deff9ab509341f264addbd3c8ada533678591905
)
FetchContent_MakeAvailable(tlsf)
message(STATUS "TLSF ready")

if (NOT TARGET tlsf)
  add_library(tlsf STATIC ${tlsf_SOURCE_DIR}/tlsf.c)
  target_include_directories(tlsf SYSTEM PUBLIC ${tlsf_SOURCE_DIR})
  message(STATUS "TLSF library created")
endif()

# ---------------- tomlplusplus ----------------
fetch_and_log("tomlplusplus")
FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)
message(STATUS "tomlplusplus ready")

if (NOT TARGET tomlplusplus)
  add_library(tomlplusplus INTERFACE)
  target_include_directories(tomlplusplus SYSTEM INTERFACE ${tomlplusplus_SOURCE_DIR}/include)
  message(STATUS "tomlplusplus INTERFACE target created")
endif()