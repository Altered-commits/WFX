include(FetchContent)

# Helper function for logging
function(fetch_and_log NAME)
    message(STATUS "Fetching and configuring ${NAME}...")
endfunction()

# ---------------- ConcurrentQueue ----------------
fetch_and_log("concurrentqueue")
FetchContent_Declare(
  concurrentqueue
  GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
  GIT_TAG        v1.0.4
)
FetchContent_MakeAvailable(concurrentqueue)
message(STATUS "ConcurrentQueue ready")

# ---------------- nlohmann/json ----------------
fetch_and_log("nlohmann/json")
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)
message(STATUS "nlohmann/json ready")

# Copy headers to include/third_party/json
set(JSON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include/third_party/json)
file(MAKE_DIRECTORY ${JSON_INCLUDE_DIR})
file(GLOB JSON_HEADERS
    ${nlohmann_json_SOURCE_DIR}/single_include/nlohmann/json.hpp
    ${nlohmann_json_SOURCE_DIR}/single_include/nlohmann/json_fwd.hpp
)
file(COPY ${JSON_HEADERS} DESTINATION ${JSON_INCLUDE_DIR})
message(STATUS "nlohmann/json headers copied to ${JSON_INCLUDE_DIR}")

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

# Copy headers to include/third_party/toml++
set(TOML_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include/third_party/toml++)
file(MAKE_DIRECTORY ${TOML_INCLUDE_DIR})
file(GLOB TOML_HEADERS ${tomlplusplus_SOURCE_DIR}/include/toml++/*.hpp)
file(COPY ${TOML_HEADERS} DESTINATION ${TOML_INCLUDE_DIR})
message(STATUS "tomlplusplus headers copied to ${TOML_INCLUDE_DIR}")