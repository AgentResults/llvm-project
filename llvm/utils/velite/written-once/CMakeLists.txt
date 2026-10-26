# velite (AgentResults): the project the check below configures. It runs AddLLVM's
# process_llvm_pass_plugins(GEN_CONFIG) as llvm/CMakeLists.txt does, with the
# static extensions EXTENSIONS names, and LLVM-Build's
# LLVMBuildGenerateCFragment, which writes llvm-config's LibraryDependencies.inc.
cmake_minimum_required(VERSION 3.20)
project(LLVMConfigExtensionsWrittenOnce C CXX)
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_MODULES}")
set(LLVM_BINARY_DIR "${CMAKE_BINARY_DIR}")
set(LLVM_LIBRARY_DIR "${CMAKE_BINARY_DIR}/lib")
set(LLVM_INSTALL_PACKAGE_DIR lib/cmake/llvm)
include(AddLLVM)
add_library(LLVMExtensions INTERFACE)
foreach(extension IN LISTS EXTENSIONS)
  add_library(${extension} INTERFACE)
  set_property(GLOBAL APPEND PROPERTY LLVM_STATIC_EXTENSIONS ${extension})
endforeach()
process_llvm_pass_plugins(GEN_CONFIG)
include(LLVM-Build)
foreach(component all-targets Engine Native NativeCodeGen)
  add_library(${component} INTERFACE)
endforeach()
set(LLVM_TARGETS_TO_BUILD "")
LLVMBuildGenerateCFragment(OUTPUT "${CMAKE_BINARY_DIR}/tools/llvm-config/LibraryDependencies.inc")
