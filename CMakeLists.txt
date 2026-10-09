cmake_minimum_required(VERSION 3.14)
project(cop5725_alex CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(NATIVE "Tune for the local CPU" OFF)
if(NATIVE)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    add_compile_options(-mcpu=native)
  else()
    add_compile_options(-march=native)
  endif()
endif()
add_compile_options(-Wall -Wextra)

add_library(tlx_die STATIC third_party/tlx/tlx/die/core.cpp)
target_include_directories(tlx_die PUBLIC third_party/tlx)

add_executable(bench src/bench.cpp)
target_include_directories(bench PRIVATE include)
target_link_libraries(bench PRIVATE tlx_die)

add_executable(tests tests/test_main.cpp)
target_include_directories(tests PRIVATE include)
target_link_libraries(tests PRIVATE tlx_die)

enable_testing()
add_test(NAME tests COMMAND tests)
