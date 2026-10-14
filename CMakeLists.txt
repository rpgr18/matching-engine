cmake_minimum_required(VERSION 3.21)

project(matching_engine
  VERSION 0.1.0
  DESCRIPTION "Price-time priority limit order book and matching engine"
  LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LOB_BUILD_TESTS "Build the unit and differential tests" ${PROJECT_IS_TOP_LEVEL})
option(LOB_BUILD_BENCH "Build the benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(LOB_WERROR "Treat compiler warnings as errors" OFF)
option(LOB_NATIVE "Tune code generation for the build machine's CPU" OFF)
set(LOB_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. 'address,undefined' or 'thread'")

include(cmake/Warnings.cmake)

# The engine itself is header-only: every hot-path call is a template
# instantiation the compiler can see through and inline.
add_library(lob INTERFACE)
add_library(lob::lob ALIAS lob)
target_include_directories(lob INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_compile_features(lob INTERFACE cxx_std_20)

if(LOB_SANITIZE)
  add_compile_options(-fsanitize=${LOB_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${LOB_SANITIZE})
endif()

if(LOB_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-march=native LOB_HAS_MARCH_NATIVE)
  check_cxx_compiler_flag(-mcpu=native LOB_HAS_MCPU_NATIVE)
  if(LOB_HAS_MARCH_NATIVE)
    add_compile_options(-march=native)
  elseif(LOB_HAS_MCPU_NATIVE)
    add_compile_options(-mcpu=native)
  endif()
endif()

if(LOB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LOB_BUILD_BENCH)
  add_subdirectory(bench)
endif()
