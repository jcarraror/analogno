cmake_minimum_required(VERSION 3.25)

project(
  analogno
  VERSION 0.1.0
  DESCRIPTION "DualSense-controlled music workstation"
  LANGUAGES CXX
)

option(ANALOGNO_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

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

add_executable(analogno
  src/main.cpp
)

target_include_directories(analogno
  PRIVATE
    src
)

target_link_libraries(analogno
  PRIVATE
    SDL3::SDL3
)

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
