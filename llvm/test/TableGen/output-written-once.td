// An output and a depfile are written to a new file and renamed onto their
// names, never written into: a file another path shares (a hard link) keeps
// what it held. With --write-if-changed, an unchanged output is untouched.
// UNSUPPORTED: system-windows

// RUN: rm -rf %t && mkdir -p %t
// RUN: llvm-tblgen -print-records -DFIRST %s -o %t/out.inc -d %t/out.d
// RUN: ln %t/out.inc %t/other.inc
// RUN: ln %t/out.d %t/other.d

// A changed output replaces the file; the other path keeps the first one.
// RUN: llvm-tblgen -print-records %s -o %t/out.inc -d %t/out.d
// RUN: FileCheck %s --check-prefix=SECOND --input-file=%t/out.inc
// RUN: FileCheck %s --check-prefix=FIRST --input-file=%t/other.inc

// A depfile, written at every run, is replaced the same way.
// RUN: llvm-tblgen -print-records %s -o %t/renamed.inc -d %t/out.d
// RUN: FileCheck %s --check-prefix=DEP-RENAMED --input-file=%t/out.d
// RUN: FileCheck %s --check-prefix=DEP-FIRST --input-file=%t/other.d

// With --write-if-changed, an unchanged output keeps its file and its time.
// RUN: touch -t 200001010000 %t/out.inc
// RUN: llvm-tblgen -print-records %s -o %t/out.inc --write-if-changed
// RUN: ls -l %t/out.inc | FileCheck %s --check-prefix=OLD-TIME

// No file is left beside them.
// RUN: ls %t | FileCheck %s --check-prefix=ONLY --implicit-check-not=temp

// FIRST: def First
// FIRST-NOT: def Second
// SECOND: def Second
// SECOND-NOT: def First
// DEP-RENAMED: {{.*}}renamed.inc:
// DEP-FIRST: {{.*}}out.inc:
// OLD-TIME: 2000
// ONLY: other.d
// ONLY: other.inc
// ONLY: out.d
// ONLY: out.inc
// ONLY: renamed.inc

#ifdef FIRST
def First;
#else
def Second;
#endif
