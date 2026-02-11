#!/usr/bin/env python3
"""Convert a binary file to a C header with byte array."""
import sys
import os

def bin2header(input_path, var_name="g_payload_dll_bytes"):
    with open(input_path, "rb") as f:
        data = f.read()

    print("#pragma once")
    print("#include <cstdint>")
    print("#include <cstddef>")
    print()
    print("// =============================================================")
    print(f"// AUTO-GENERATED from: {os.path.basename(input_path)}")
    print(f"// Size: {len(data)} bytes")
    print("// =============================================================")
    print()
    print(f"static const unsigned char {var_name}[] = {{")

    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
        print(f"    {hex_str},")

    print("};")
    print()
    print(f"static const size_t {var_name[:-6]}_size = sizeof({var_name});")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <input.dll> [variable_name]", file=sys.stderr)
        sys.exit(1)
    var = sys.argv[2] if len(sys.argv) > 2 else "g_payload_dll_bytes"
    bin2header(sys.argv[1], var)
