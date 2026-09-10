cmake_minimum_required(VERSION 3.20)
project(dariyanaap LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# For clangd / IDE navigation.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Vendoring
# ---------------------------------------------------------------------------
#
# Unlike every other repo in the track, dariyanaap is mostly consumed as a
# submodule (`add_subdirectory(vendor/dariyanaap)`). So it must behave when it
# is *not* the top-level project: no test targets, no CLI, no sanitizer flags
# forced on its parent. Everything conditional hangs off this one variable.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(DARIYANAAP_IS_TOP_LEVEL ON)
else()
  set(DARIYANAAP_IS_TOP_LEVEL OFF)
endif()

option(DARIYANAAP_BUILD_TESTS "Build the test suites" ${DARIYANAAP_IS_TOP_LEVEL})
option(DARIYANAAP_BUILD_CLI   "Build the dariyanaap CLI" ${DARIYANAAP_IS_TOP_LEVEL})
option(DARIYANAAP_ASAN        "Build with AddressSanitizer" OFF)
option(DARIYANAAP_TSAN        "Build with ThreadSanitizer"  OFF)

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------
#
# TSan is the one that matters here: the load driver keeps a histogram per
# thread precisely so nothing is shared on the hot path, and that claim is only
# worth anything if a race detector agrees with it.

if(DARIYANAAP_ASAN AND DARIYANAAP_TSAN)
  message(FATAL_ERROR "DARIYANAAP_ASAN and DARIYANAAP_TSAN cannot both be enabled")
endif()
# Sanitizer builds keep assert() live. RelWithDebInfo — the default, and what
# the measurement build uses — defines NDEBUG, which compiles every assert in
# the repo away. That is right for a run whose numbers must not pay for
# checks, and wrong for the builds whose entire job is finding bugs: the
# asserts guarding "the caller mixed up two clocks" and "read_some on a
# moved-from socket" were documentation rather than enforcement until this
# existed. -UNDEBUG comes after the build-type flags, so it wins.
if(DARIYANAAP_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -UNDEBUG)
  add_link_options(-fsanitize=address,undefined)
endif()
if(DARIYANAAP_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -UNDEBUG)
  add_link_options(-fsanitize=thread)
endif()

# Warnings are errors when dariyanaap is the project being worked on, and only
# then. Not strictness for its own sake — an incremental build re-emits nothing
# for an unchanged file, so a warning that merely prints is caught once and
# then invisible on every subsequent build. -Werror makes the object file fail
# to exist, so the failure repeats until it is fixed.
#
# Off by default when vendored, so our strictness cannot break a parent repo
# built with a newer compiler that warns about something we never saw.
option(DARIYANAAP_WERROR "Treat warnings as errors" ${DARIYANAAP_IS_TOP_LEVEL})

set(DARIYANAAP_WARNINGS -Wall -Wextra -Wpedantic)
if(DARIYANAAP_WERROR)
  list(APPEND DARIYANAAP_WARNINGS -Werror)
endif()

# ---------------------------------------------------------------------------
# Library: core
# ---------------------------------------------------------------------------
#
# Units, the monotonic clock, and errors. Depends on nothing. Includes are
# rooted at src/, so every include reads "core/clock.hpp" and the layer a
# header belongs to is visible at the include site.

add_library(dariyanaap_core
  src/core/version.cpp
  src/core/errors.cpp
  src/core/units.cpp
  src/core/clock.cpp
  src/core/endpoint.cpp
  src/core/socket.cpp
  src/core/address.cpp
  src/core/listener.cpp
)
target_include_directories(dariyanaap_core PUBLIC src)
target_compile_options(dariyanaap_core PRIVATE ${DARIYANAAP_WARNINGS})
add_library(dariyanaap::core ALIAS dariyanaap_core)

# ---------------------------------------------------------------------------
# Library: stats
# ---------------------------------------------------------------------------
#
# Depends on core and nothing else. buckets.hpp and Histogram::record stay in
# headers on purpose — they are constexpr arithmetic and the hot path, and a
# call across a translation-unit boundary per sample would be measurement
# overhead. Everything colder than that lives in a .cpp.

add_library(dariyanaap_stats
  src/stats/histogram.cpp
  src/stats/summary.cpp
  src/stats/csv.cpp
)
target_link_libraries(dariyanaap_stats PUBLIC dariyanaap_core)
target_compile_options(dariyanaap_stats PRIVATE ${DARIYANAAP_WARNINGS})
add_library(dariyanaap::stats ALIAS dariyanaap_stats)

# ---------------------------------------------------------------------------
# Library: load
# ---------------------------------------------------------------------------
#
# The driver: protocols and the runners. Depends on stats, because a runner
# records into a Histogram, and transitively on core for sockets.

add_library(dariyanaap_load
  src/load/raw_echo.cpp
  src/load/http.cpp
  src/load/http11_get.cpp
  src/load/exchange.cpp
  src/load/worker.cpp
)
target_link_libraries(dariyanaap_load PUBLIC dariyanaap_stats)
target_compile_options(dariyanaap_load PRIVATE ${DARIYANAAP_WARNINGS})
add_library(dariyanaap::load ALIAS dariyanaap_load)

# M4 adds dariyanaap_fault.
# Each gets its own block here, and the layering is enforced by the build:
# if core ever includes from load, it will not link.

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
#
# hist_verify is M1's benchmark: track rule 3 says a unit without a number is
# unfinished. Guarded by BUILD_CLI so vendoring dariyanaap into another repo
# does not build it.

if(DARIYANAAP_BUILD_CLI)
  add_executable(hist_verify apps/hist_verify.cpp)
  target_link_libraries(hist_verify PRIVATE dariyanaap_stats)
  target_compile_options(hist_verify PRIVATE ${DARIYANAAP_WARNINGS})
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(DARIYANAAP_BUILD_TESTS)
  include(FetchContent)

  # doctest is header-only, so we fetch the source and use the header directly
  # rather than building doctest's own CMake project. SOURCE_SUBDIR names a
  # directory with no CMakeLists.txt, which tells FetchContent_MakeAvailable to
  # download without calling add_subdirectory().
  #
  # This is not only tidier, it is necessary: doctest v2.4.11 declares
  # cmake_minimum_required(VERSION 3.0), CMake 4 dropped compatibility with
  # anything below 3.5, and configuring its project fails outright. (Same fix
  # as dariyakyu.)
  FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    SOURCE_SUBDIR  header-only-no-cmake-project
  )
  FetchContent_MakeAvailable(doctest)

  add_library(doctest_headers INTERFACE)
  target_include_directories(doctest_headers INTERFACE ${doctest_SOURCE_DIR})
  add_library(doctest::doctest ALIAS doctest_headers)

  enable_testing()

  foreach(suite core_tests stats_tests load_tests)
    add_executable(${suite} tests/${suite}.cpp)
    target_link_libraries(${suite} PRIVATE dariyanaap_load doctest::doctest)
    target_include_directories(${suite} PRIVATE tests)
    target_compile_options(${suite} PRIVATE ${DARIYANAAP_WARNINGS})
    add_test(NAME ${suite} COMMAND ${suite})
  endforeach()
endif()
