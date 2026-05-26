#!/usr/bin/env python3
"""Embed a SPIR-V binary into a C header as a uint32_t array."""

import pathlib
import sys


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        sys.stderr.write("usage: embed_spv.py <input.spv> <output.h> <symbol>\n")
        return 2

    inp = pathlib.Path(argv[1])
    out = pathlib.Path(argv[2])
    name = argv[3]

    data = inp.read_bytes()
    if len(data) % 4 != 0:
        sys.stderr.write(f"{inp}: not a multiple of 4 bytes ({len(data)})\n")
        return 1

    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]

    lines = [
        f"/* Auto-generated from {inp.name}. Do not edit. */",
        "#pragma once",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        f"static const uint32_t {name}[] = {{",
    ]

    per_line = 8
    for i in range(0, len(words), per_line):
        chunk = ", ".join(f"0x{w:08x}" for w in words[i:i + per_line])
        lines.append(f"    {chunk},")

    lines += [
        "};",
        f"static const size_t {name}_size = sizeof({name});",
        "",
    ]

    out.write_text("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
