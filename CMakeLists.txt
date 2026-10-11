cmake_minimum_required(VERSION 3.20)

# Use the Homebrew LLVM installation (never Apple's clang). The compilers
# must be chosen before project().
set(SENTINEL_LLVM_PREFIX "/opt/homebrew/opt/llvm" CACHE PATH "LLVM install prefix")
if(NOT CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER "${SENTINEL_LLVM_PREFIX}/bin/clang")
endif()
if(NOT CMAKE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER "${SENTINEL_LLVM_PREFIX}/bin/clang++")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

project(SentinEL C CXX)   # C is needed: LLVMConfig.cmake runs a C header check
list(APPEND CMAKE_PREFIX_PATH "${SENTINEL_LLVM_PREFIX}")

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
separate_arguments(LLVM_DEFS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_definitions(${LLVM_DEFS})
if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()

# Plugins are loaded *into* the clang process. clang already contains
# libLLVM / libclang-cpp, so we do NOT link them again; instead we tell the
# macOS linker to resolve those symbols at load time.
function(sentinel_plugin name)
  add_library(${name} MODULE ${ARGN})
  set_target_properties(${name} PROPERTIES
    PREFIX ""
    SUFFIX ".dylib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
  if(APPLE)
    target_link_options(${name} PRIVATE -undefined dynamic_lookup)
  endif()
endfunction()

sentinel_plugin(SentinelAST ast-plugin/SentinelAST.cpp)
sentinel_plugin(SentinelIR  ir-pass/SentinelIR.cpp)
