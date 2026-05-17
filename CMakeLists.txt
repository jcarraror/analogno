cmake_minimum_required(VERSION 3.25)

project(
  analogno
  VERSION 0.1.0
  DESCRIPTION "DualSense-controlled music workstation"
  LANGUAGES CXX
)

option(ANALOGNO_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(ANALOGNO_ENABLE_AUBIO "Enable aubio-backed voice-to-sequencer analysis" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
  SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.2.26
  GIT_SHALLOW TRUE
)

set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(SDL3)

FetchContent_Declare(
  rtmidi
  GIT_REPOSITORY https://github.com/thestk/rtmidi.git
  GIT_TAG 6.0.0
  GIT_SHALLOW TRUE
)

set(RTMIDI_API_ALSA ON CACHE BOOL "" FORCE)
set(RTMIDI_API_JACK OFF CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(rtmidi)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
  miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.22
  GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(miniaudio)

find_package(Boost REQUIRED)

if (ANALOGNO_ENABLE_AUBIO)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(AUBIO QUIET IMPORTED_TARGET aubio)
  if (NOT AUBIO_FOUND)
    message(WARNING "aubio was not found; voice-to-sequencer will be compiled unavailable")
  endif()
endif()

add_executable(analogno
  src/main.cpp
  src/controller_state.cpp
  src/music_mapper.cpp
  src/midi_output.cpp
  src/runtime_state_builder.cpp
  src/sequencer.cpp
  src/web_server.cpp
  src/audio_capture.cpp
  src/audio_sampler.cpp
  src/voice_sequencer.cpp
  src/miniaudio_impl.cpp
)

target_include_directories(analogno
  PRIVATE
    src
)

target_include_directories(analogno
  SYSTEM PRIVATE
    ${miniaudio_SOURCE_DIR}
)

target_link_libraries(analogno
  PRIVATE
    SDL3::SDL3
    rtmidi
    nlohmann_json::nlohmann_json
    Boost::headers
    pthread
)

if (ANALOGNO_ENABLE_AUBIO AND AUBIO_FOUND)
  target_link_libraries(analogno PRIVATE PkgConfig::AUBIO)
  target_compile_definitions(analogno PRIVATE ANALOGNO_HAVE_AUBIO=1)
else()
  target_compile_definitions(analogno PRIVATE ANALOGNO_HAVE_AUBIO=0)
endif()

target_compile_features(analogno
  PRIVATE
    cxx_std_23
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(analogno PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
  )

  if (ANALOGNO_WARNINGS_AS_ERRORS)
    target_compile_options(analogno PRIVATE -Werror)
  endif()
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_options(analogno PRIVATE
    -Wimplicit-fallthrough
  )
endif()
