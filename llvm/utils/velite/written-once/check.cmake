# velite (AgentResults): LLVMConfigExtensions.cmake is written once, never into.
#
#   cmake -DWORK=<an empty directory> -P llvm/utils/velite/written-once/check.cmake
#
# A configure that writes the same content leaves the file untouched (its inode
# and time), and one that writes a new content replaces it with a new file, so a
# path that shares the old file (a hard link from an earlier build's place)
# keeps it. Run it with a CMake whose file(WRITE) writes into the file it names
# (CMake's own, through 4.1): AddLLVM.cmake's write must not depend on the
# CMake that runs it.
if(NOT WORK)
  message(FATAL_ERROR "name an empty directory: -DWORK=<dir>")
endif()
file(REMOVE_RECURSE "${WORK}")
get_filename_component(modules "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/modules" ABSOLUTE)
set(build "${WORK}/build")
set(file "${build}/lib/cmake/llvm/LLVMConfigExtensions.cmake")
set(other "${WORK}/other.cmake")

function(configure extensions)
  execute_process(COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}"
    -B "${build}" "-DLLVM_CMAKE_MODULES=${modules}" "-DEXTENSIONS=${extensions}"
    RESULT_VARIABLE r OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT r EQUAL 0)
    message(FATAL_ERROR "configure answered ${r}:\n${out}")
  endif()
endfunction()

function(expect_content path content)
  file(READ "${path}" actual)
  if(NOT actual STREQUAL content)
    message(SEND_ERROR "${path} holds '${actual}', not '${content}'.")
  endif()
endfunction()

configure("")
expect_content("${file}" "set(LLVM_STATIC_EXTENSIONS )")
file(CREATE_LINK "${file}" "${other}")
execute_process(COMMAND touch -t 200001010000 "${file}")

# The same content: the file is untouched.
configure("")
file(TIMESTAMP "${file}" year "%Y")
if(NOT year STREQUAL "2000")
  message(SEND_ERROR "${file} was written again with the same content: its time is of ${year}, not 2000.")
endif()

# A new content: a new file; the path that shares the old one keeps it.
configure("Bye")
expect_content("${file}" "set(LLVM_STATIC_EXTENSIONS Bye)")
expect_content("${other}" "set(LLVM_STATIC_EXTENSIONS )")
