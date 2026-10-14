cmake_minimum_required(VERSION 3.21)  # PROJECT_IS_TOP_LEVEL
project(order_book LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LOB_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(LOB_BUILD_TOOLS "Build the benchmark and command line tools" ${PROJECT_IS_TOP_LEVEL})
option(LOB_SANITIZE "Build with ASan and UBSan" OFF)
option(LOB_NATIVE "Compile for the host CPU (-march=native)" OFF)
option(LOB_WERROR "Treat warnings as errors (CI)" OFF)

# The library itself is header-only. Consumers get the include path and C++20,
# nothing else; warnings and sanitizers stay on this project's own targets.
add_library(lob INTERFACE)
add_library(lob::lob ALIAS lob)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob INTERFACE cxx_std_20)

add_library(lob_dev INTERFACE)
target_link_libraries(lob_dev INTERFACE lob)
if(MSVC)
  target_compile_options(lob_dev INTERFACE /W4 /permissive- $<$<BOOL:${LOB_WERROR}>:/WX>)
else()
  target_compile_options(lob_dev INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
  if(LOB_WERROR)
    target_compile_options(lob_dev INTERFACE -Werror)
  endif()
  if(LOB_SANITIZE)
    target_compile_options(lob_dev INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer
                                             -fno-sanitize-recover=all)
    target_link_options(lob_dev INTERFACE -fsanitize=address,undefined)
  endif()
  if(LOB_NATIVE)
    target_compile_options(lob_dev INTERFACE -march=native)
  endif()
endif()

if(LOB_BUILD_TOOLS)
  add_executable(lob_bench bench/bench.cpp)
  target_link_libraries(lob_bench PRIVATE lob_dev)

  add_executable(lob_replay tools/replay.cpp)
  target_link_libraries(lob_replay PRIVATE lob_dev)

  add_executable(lob_itch_replay tools/itch_replay.cpp)
  target_link_libraries(lob_itch_replay PRIVATE lob_dev)

  add_executable(lob_make_itch tools/make_itch.cpp)
  target_link_libraries(lob_make_itch PRIVATE lob_dev)
endif()

if(LOB_BUILD_TESTS)
  include(FetchContent)
  # Pinned to the commit behind v1.15.2 so a moved tag can't change the build.
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59  # v1.15.2
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lob_tests
    tests/test_order_book.cpp
    tests/test_id_map.cpp
    tests/test_fuzz.cpp
    tests/test_bitmap.cpp
    tests/test_edge_cases.cpp
    tests/test_itch.cpp
  )
  target_link_libraries(lob_tests PRIVATE lob_dev GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(lob_tests DISCOVERY_TIMEOUT 60)
endif()
