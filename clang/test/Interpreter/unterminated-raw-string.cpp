// REQUIRES: host-supports-jit
// An input holding an unterminated raw string literal inside a compound
// statement must end in an error, never parse without end: at the end of a
// repl input the preprocessor yields annot_repl_input_end, not eof, and the
// body loops test isEofOrEom.
// RUN: cat %s | not clang-repl 2>&1 | FileCheck %s

void f() { const char *s = R"(never closed; }
// CHECK: unterminated raw string
