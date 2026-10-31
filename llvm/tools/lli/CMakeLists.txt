if ( LLVM_INCLUDE_UTILS )
  add_subdirectory(ChildTarget)
endif()

set(LLVM_LINK_COMPONENTS
  CodeGen
  Core
  ExecutionEngine
  IRReader
  Interpreter
  JITLink
  MC
  MCJIT
  Object
  OrcJIT
  OrcDebugging
  OrcShared
  OrcTargetProcess
  Passes
  RuntimeDyld
  SelectionDAG
  Support
  Target
  TargetParser
  TransformUtils
  native
  )

if( LLVM_USE_OPROFILE )
  set(LLVM_LINK_COMPONENTS
    ${LLVM_LINK_COMPONENTS}
    OProfileJIT
    )
endif( LLVM_USE_OPROFILE )

if( LLVM_USE_INTEL_JITEVENTS )
  set(LLVM_LINK_COMPONENTS
    ${LLVM_LINK_COMPONENTS}
    DebugInfoDWARF
    IntelJITEvents
    Object
    )
endif( LLVM_USE_INTEL_JITEVENTS )

if( LLVM_USE_PERF )
  set(LLVM_LINK_COMPONENTS
    ${LLVM_LINK_COMPONENTS}
    DebugInfoDWARF
    PerfJITEvents
    Object
    )
endif( LLVM_USE_PERF )

if (MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY AND
    (NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL$"))
  # Do not export symbols when linking with the static CRT because in this case,
  # only a subset of CRT symbols is exported. When executing a module, this can
  # cause imported CRT symbols to resolve to different CRT instances. For
  # example, 'lli.exe' may export 'printf' but not 'fflush' or 'exit', causing
  # 'printf' to bind to the static CRT while the latter resolve to the dynamic
  # CRT. Disabling symbol export forces all imports to bind to the dynamic CRT
  # instance.
  set(MAYBE_EXPORT_SYMBOLS "")
else()
  set(MAYBE_EXPORT_SYMBOLS EXPORT_SYMBOLS)
endif()

# lli's tool is a library, liblli, exporting `lli_main` (llvm-driver's `<tool>_main`, lli.cpp); the lli executable is the
# `main` that calls it (lli-main.cpp). A process that is not lli runs lli's tool by loading the library.
add_library(lli_library SHARED lli.cpp)
llvm_update_compile_flags(lli_library)
add_dependencies(lli_library intrinsics_gen)
if(LLVM_LINK_LLVM_DYLIB)
  set(LLI_USE_SHARED USE_SHARED)
endif()
llvm_config(lli_library ${LLI_USE_SHARED} ${LLVM_LINK_COMPONENTS})
set_target_properties(lli_library PROPERTIES OUTPUT_NAME lli)
set_output_directory(lli_library BINARY_DIR ${LLVM_RUNTIME_OUTPUT_INTDIR} LIBRARY_DIR ${LLVM_LIBRARY_OUTPUT_INTDIR})
if(APPLE)
  set_target_properties(lli_library PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "@loader_path"
    INSTALL_NAME_DIR "@rpath")
endif()
install(TARGETS lli_library
  LIBRARY DESTINATION lib${LLVM_LIBDIR_SUFFIX} COMPONENT lli
  RUNTIME DESTINATION bin COMPONENT lli)

# The directory's other source, lli.cpp, is liblli's (lli_library, above).
add_llvm_tool(lli
  lli-main.cpp
  PARTIAL_SOURCES_INTENDED

  DEPENDS
  intrinsics_gen

  ${MAYBE_EXPORT_SYMBOLS}
  )
target_link_libraries(lli PRIVATE lli_library)
