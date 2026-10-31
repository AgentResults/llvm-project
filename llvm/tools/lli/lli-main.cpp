//===- lli-main.cpp - the lli executable ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// lli's tool is the library liblli (lli.cpp), whose `lli_main` is llvm-driver's
// `<tool>_main`; this executable only calls it.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/LLVMDriver.h"

int lli_main(int argc, char **argv, const llvm::ToolContext &);

int main(int argc, char **argv) {
  return lli_main(argc, argv, {argv[0], nullptr, false});
}
