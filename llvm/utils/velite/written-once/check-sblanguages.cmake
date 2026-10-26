# velite (AgentResults): lldb's SBLanguages.h, which lldb lays among its public
# headers, is written once, never into.
#
#   cmake -DWORK=<an empty directory> -P llvm/utils/velite/written-once/check-sblanguages.cmake
#
# Its generator run again with the same Dwarf.def leaves the header untouched
# (its time), so nothing that includes it compiles again; run with a changed one
# it replaces the header with a new file, so a path that shares the old one (a
# hard link from an earlier build's place) keeps it, and leaves no file beside it.
if(NOT WORK)
  message(FATAL_ERROR "name an empty directory: -DWORK=<dir>")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/out")
find_program(PYTHON3 python3 REQUIRED)
get_filename_component(generator "${CMAKE_CURRENT_LIST_DIR}/../../../../lldb/scripts/generate-sbapi-dwarf-enum.py" ABSOLUTE)
set(header "${WORK}/out/SBLanguages.h")
set(other "${WORK}/other.h")

function(generate names)
  set(def "")
  foreach(name IN LISTS names)
    string(APPEND def "HANDLE_DW_LNAME(0x0001, ${name}, \"${name}\", 0)\n")
  endforeach()
  file(WRITE "${WORK}/Dwarf.def" "${def}")
  execute_process(COMMAND "${PYTHON3}" "${generator}" "${WORK}/Dwarf.def" -o "${header}"
    RESULT_VARIABLE r ERROR_VARIABLE err)
  if(NOT r EQUAL 0)
    message(FATAL_ERROR "the generator answered ${r}: ${err}")
  endif()
endfunction()

generate("First")
file(CREATE_LINK "${header}" "${other}")
execute_process(COMMAND touch -t 200001010000 "${header}")
generate("First")
file(TIMESTAMP "${header}" year "%Y")
if(NOT year STREQUAL "2000")
  message(SEND_ERROR "${header} was written again with the same content: its time is of ${year}, not 2000.")
endif()
generate("Second")
file(READ "${header}" now)
file(READ "${other}" before)
if(NOT now MATCHES "eLanguageNameSecond")
  message(SEND_ERROR "${header} does not hold the new content:\n${now}")
endif()
if(NOT before MATCHES "eLanguageNameFirst" OR before MATCHES "eLanguageNameSecond")
  message(SEND_ERROR "${other}, which shared the header, was written into:\n${before}")
endif()
file(GLOB left RELATIVE "${WORK}/out" "${WORK}/out/*")
if(NOT left STREQUAL "SBLanguages.h")
  message(SEND_ERROR "the header's directory holds '${left}', not 'SBLanguages.h'.")
endif()
