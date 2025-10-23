include(FetchContent)

# Helper function for logging
function(fetch_and_log NAME)
    message(STATUS "Fetching and configuring ${NAME}...")
endfunction()

# ---------------- nlohmann/json ----------------
# This approach makes it properly available for both Engine and User
fetch_and_log("nlohmann/json")
set(JSON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include/third_party/json)
file(MAKE_DIRECTORY ${JSON_INCLUDE_DIR})

# URLs of the single-header files, v3.12.0
set(JSON_HPP_URL "https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp")
set(JSON_FWD_HPP_URL "https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json_fwd.hpp")

# Download headers only if they don't exist
if(NOT EXISTS "${JSON_INCLUDE_DIR}/json.hpp")
  file(DOWNLOAD ${JSON_HPP_URL} "${JSON_INCLUDE_DIR}/json.hpp" SHOW_PROGRESS)
endif()

if(NOT EXISTS "${JSON_INCLUDE_DIR}/json_fwd.hpp")
  file(DOWNLOAD ${JSON_FWD_HPP_URL} "${JSON_INCLUDE_DIR}/json_fwd.hpp" SHOW_PROGRESS)
endif()

message(STATUS "nlohmann/json ready in ${JSON_INCLUDE_DIR}")

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
  target_include_directories(tlsf PUBLIC ${tlsf_SOURCE_DIR})
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
  target_include_directories(tomlplusplus INTERFACE ${tomlplusplus_SOURCE_DIR}/include)
  message(STATUS "tomlplusplus INTERFACE target created")
endif()