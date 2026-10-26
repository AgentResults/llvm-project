#!/usr/bin/env python3

import argparse
import re
import os

HEADER = """\
//===-- SBLanguages.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_API_SBLANGUAGE_H
#define LLDB_API_SBLANGUAGE_H

#include <cstdint>

namespace lldb {
/// Used by \\ref SBExpressionOptions.
/// These enumerations use the same language enumerations as the DWARF
/// specification for ease of use and consistency.
enum SBSourceLanguageName : uint16_t {
"""

FOOTER = """\
};

} // namespace lldb

#endif
"""

REGEX = re.compile(
    r'^ *HANDLE_DW_LNAME *\( *(?P<value>[^,]+), (?P<name>.*), "(?P<comment>[^"]+)",.*\)'
)


def emit_enum(input, output):
    # Read the input and break it up by lines.
    lines = []
    with open(input, "r") as f:
        lines = f.readlines()

    # Create output folder if it does not exist
    os.makedirs(os.path.dirname(output), exist_ok=True)

    # The output.
    text = HEADER
    for line in lines:
        match = REGEX.match(line)
        if not match:
            continue
        text += f"  /// {match.group('comment')}.\n"
        text += f"  eLanguageName{match.group('name')} = {match.group('value')},\n"
    text += FOOTER

    # velite (AgentResults): written once, never into. An output that already
    # holds this text is left untouched (its inode and time), so nothing that
    # includes it compiles again; another is written to a new file beside it and
    # renamed onto it, so a path that shares the old file (a hard link from an
    # earlier build's place) is never changed.
    try:
        with open(output, "r") as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    temporary = f"{output}.tmp{os.getpid()}"
    with open(temporary, "w") as f:
        f.write(text)
    os.replace(temporary, output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", "-o")
    parser.add_argument("input")
    args = parser.parse_args()

    emit_enum(args.input, args.output)


if __name__ == "__main__":
    main()
