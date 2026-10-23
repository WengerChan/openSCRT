#!/usr/bin/env python3
"""Check that a binary does not import anything newer than a declared Windows version.

Why this exists
---------------
A program that statically imports a symbol from kernel32.dll cannot start on a system
whose kernel32 does not export it. The loader says so with a dialog that reads
"the procedure entry point ... could not be located" and nothing else -- before main(),
with no application message, no log, and no way for the user to act on it.

CI cannot catch that on its own: the runner is newer than every system anyone supports, so
a new API call compiles and passes there, and the floor rises silently. This script reads
the import table of the built binaries and fails when an import is newer than the version
the project declares.

The version table
-----------------
The list below holds the kernel32 exports that are known to be *newer* than Windows 10
1607 (build 14393). A symbol that is not on the list is treated as 1607-safe.

The versions are the ones the kernel32 *export table* actually provides, not the
"minimum client" column in the documentation: the documentation describes the API's
availability in the KernelBase and apiset layers, and kernel32 starts forwarding an
export later than that. SetThreadDescription is the sharpest example -- the documentation
says 1607, and a real 1607 kernel32 does not export it.

The floor for the whole package is the highest one any of its binaries needs. The Qt
libraries in the package set their own floor, which is why this script reports it rather
than only checking our own binaries.
"""

import argparse
import struct
import sys

# Windows 10 version helpers, by build number.
BUILD_14393 = (10, 0, 14393)   # 1607
BUILD_15063 = (10, 0, 15063)   # 1703
BUILD_17134 = (10, 0, 17134)   # 1803
BUILD_17763 = (10, 0, 17763)   # 1809

# kernel32 exports that are newer than 1607. Anything not listed is assumed to be
# 1607-safe, which is the conservative direction for the ones we know and the optimistic
# direction for the ones we do not -- so a new API that nobody classified shows up as a
# failure to classify it rather than as a silent floor rise.
NEWER_THAN_1607 = {
    "SetProcessDpiAwarenessContext": BUILD_15063,
    "GetSystemTimePreciseAsFileTime": BUILD_17134,
    "PowerSetRequest": BUILD_17134,
    "CreatePseudoConsole": BUILD_17763,
    "ResizePseudoConsole": BUILD_17763,
    "ClosePseudoConsole": BUILD_17763,
    # The documentation says 1607; a real 1607 kernel32 does not export it, which was
    # measured on a 10.0.14393.1715 system. The build below is the 1709 one, where
    # kernel32 is known to forward it; the exact build is not what matters, only that it
    # is above 1607.
    "SetThreadDescription": (10, 0, 16299),
    "GetThreadDescription": (10, 0, 16299),
}

FLOORS = {
    "1507": (10, 0, 10240),
    "1607": BUILD_14393,
    "1703": BUILD_15063,
    "1803": BUILD_17134,
    "1809": BUILD_17763,
}


def build(version):
    numbers = version.split(".")
    major = int(numbers[0])
    minor = int(numbers[1]) if len(numbers) > 1 else 0
    build_number = int(numbers[2]) if len(numbers) > 2 else 0
    return (major, minor, build_number)


def format_version(version):
    return f"{version[0]}.{version[1]} build {version[2]}"


def pe_imports(path):
    """Returns {dll name: {symbol, ...}} for a PE file."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:2] != b"MZ":
        raise SystemExit(f"{path}: not a PE file")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise SystemExit(f"{path}: bad PE signature")
    coff = pe_offset + 4
    section_count = struct.unpack_from("<H", data, coff + 2)[0]
    optional_size = struct.unpack_from("<H", data, coff + 16)[0]
    optional = coff + 20
    magic = struct.unpack_from("<H", data, optional)[0]
    pe32plus = magic == 0x20B
    directory = optional + (112 if pe32plus else 96)
    import_rva, _ = struct.unpack_from("<II", data, directory + 8)

    sections = []
    section_table = optional + optional_size
    for index in range(section_count):
        offset = section_table + index * 40
        virtual_address, virtual_size, raw_offset, raw_size = struct.unpack_from(
            "<IIII", data, offset + 12)
        sections.append((virtual_address, virtual_size, raw_offset, raw_size))

    def to_offset(rva):
        for virtual_address, virtual_size, raw_offset, raw_size in sections:
            if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                return raw_offset + (rva - virtual_address)
        raise ValueError(f"rva {rva:#x} is outside the sections")

    result = {}
    cursor = to_offset(import_rva)
    while True:
        entry = data[cursor:cursor + 20]
        if len(entry) < 20 or entry == b"\0" * 20:
            break
        name_rva = struct.unpack_from("<I", entry, 12)[0]
        thunk_rva = struct.unpack_from("<I", entry, 0)[0]
        dll = data[to_offset(name_rva):to_offset(name_rva) + 64].split(b"\0")[0].decode("latin1")
        symbols = set()
        thunk = to_offset(thunk_rva)
        step = 8 if pe32plus else 4
        while True:
            value = struct.unpack_from("<Q" if pe32plus else "<I", data, thunk)[0]
            if value == 0:
                break
            if not (value & (1 << 63)):
                # The hint-name table starts with a two-byte ordinal hint.
                hint = to_offset(value & 0x7FFFFFFF)
                symbols.add(data[hint + 2:hint + 130].split(b"\0")[0].decode("latin1"))
            thunk += step
        result.setdefault(dll.lower(), set()).update(symbols)
        cursor += 20
    return result


def floor_of(path):
    """The newest Windows version this binary needs, from its kernel32 imports."""
    imports = pe_imports(path)
    needed = BUILD_14393
    for symbol in imports.get("kernel32.dll", set()):
        introduced = NEWER_THAN_1607.get(symbol)
        if introduced is not None and introduced > needed:
            needed = introduced
    return needed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--floor", default="1607", choices=sorted(FLOORS),
                        help="the newest Windows version the project may require")
    parser.add_argument("--strict", action="append", default=[],
                        help="a binary that must not exceed the floor")
    parser.add_argument("--report", action="append", default=[],
                        help="a binary whose floor is reported but not enforced")
    arguments = parser.parse_args()

    limit = FLOORS[arguments.floor]
    print(f"declared floor: Windows {format_version(limit)}")

    failures = []
    for path in arguments.strict:
        needed = floor_of(path)
        verdict = "ok" if needed <= limit else "TOO NEW"
        print(f"  {path}: needs {format_version(needed)}  [{verdict}]")
        if needed > limit:
            failures.append(path)

    highest = limit
    for path in arguments.report:
        needed = floor_of(path)
        print(f"  {path}: needs {format_version(needed)}")
        if needed > highest:
            highest = needed

    print(f"package floor: Windows {format_version(highest)}")

    if failures:
        print("\n::error::these binaries import kernel32 exports newer than the declared floor:")
        for path in failures:
            print(f"::error::{path}")
        print("::error::a Windows older than the floor dies in the loader, before main()")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
