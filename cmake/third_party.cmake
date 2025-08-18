include(FetchContent)

# ---------------- ConcurrentQueue ----------------
FetchContent_Declare(
  concurrentqueue
  GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
  GIT_TAG        v1.0.4
)
FetchContent_MakeAvailable(concurrentqueue)

if (NOT TARGET concurrentqueue)
  add_library(concurrentqueue INTERFACE)
  target_include_directories(concurrentqueue INTERFACE
      ${concurrentqueue_SOURCE_DIR}
  )
endif()

# ---------------- nlohmann/json ----------------
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)
# note: this repo *already defines* a target called nlohmann_json,
# so nothing else is needed.

# ---------------- TLSF ----------------
FetchContent_Declare(
  tlsf
  GIT_REPOSITORY https://github.com/mattconte/tlsf.git
  GIT_TAG        deff9ab509341f264addbd3c8ada533678591905
)
FetchContent_MakeAvailable(tlsf)

if (NOT TARGET tlsf)
  add_library(tlsf STATIC
      ${tlsf_SOURCE_DIR}/tlsf.c
  )
  target_include_directories(tlsf PUBLIC ${tlsf_SOURCE_DIR})
endif()

# ---------------- tomlplusplus ----------------
FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

if (NOT TARGET tomlplusplus)
  add_library(tomlplusplus INTERFACE)
  target_include_directories(tomlplusplus INTERFACE
      ${tomlplusplus_SOURCE_DIR}/include
  )
endif()