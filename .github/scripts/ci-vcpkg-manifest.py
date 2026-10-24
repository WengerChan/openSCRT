#!/usr/bin/env python3
"""Derive the CI vcpkg manifest from vcpkg.json.

`vcpkg.json` is the single source of truth for dependencies (see
docs/DEVELOPMENT_PLAN.md, decision D4). CI cannot install all of it: some ports are provisioned
elsewhere (Qt comes from jurplel/install-qt-action, never vcpkg) and some are not linked yet. A
port that nothing links is a source build nobody waits for -- on windows-2022 the full manifest
was 74.6 min of a 75.6 min job.

Both workflows call this so the skip table has exactly one home:

    ci.yml      python .github/scripts/ci-vcpkg-manifest.py <out> --with-tests
    release.yml python .github/scripts/ci-vcpkg-manifest.py <out>

Writes `<out>/vcpkg.json` (pointed at by VCPKG_MANIFEST_DIR so vcpkg.cmake computes the installed
tree prefix) and `<out>/ports.txt` (the explicit port list for a classic-mode install).

Contract: when a phase starts linking a port, delete its SKIP entry. A missing port fails fast at
configure time ("not found") instead of silently compiling something nobody wanted.
"""

import argparse
import json
import os
import sys

# port -> why CI does not install it.
# A port belongs here only while nothing links it.
SKIP = {
    "qtbase": "Qt6 comes from install-qt-action; a vcpkg qtbase build is hours",
    "qtserialport": "same Qt prebuilt package as qtbase",
    "pybind11": "pulls vcpkg's `python3`, a CPython source build worth +20 min (Phase 5)",
    # libssh is no longer skipped: P1-T11 links it. vcpkg pulls openssl in as a transitive
    # dependency of libssh, so openssl still gets built even though nothing links it
    # directly -- that is the cost the D4 contract accepts when a phase starts using a port.
    #
    # openssl and zlib are no longer skipped either: P2-T4 links openssl for the KDBX
    # cryptography (SHA-256, HMAC-SHA256, AES-256-CBC) and zlib for the GZip-compressed
    # payload. openssl was already being built as libssh's dependency, so this costs no
    # extra build time; zlib is a small build of its own.
    #
    # pugixml is no longer skipped either: P2-T6 links it for the XML inside a KeePass
    # database. nlohmann-json is no longer skipped: P2-T8 links it for the KeePassHTTP
    # payloads.
}

# Only linked by the test tree. A packaging/release build configures with
# OPENCRT_BUILD_TESTS=OFF, so building gtest would be dead weight in the job that ships the
# installer.
TESTS_ONLY = {
    "gtest": "only the test tree links it; drop it when OPENCRT_BUILD_TESTS=OFF",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out_dir", help="directory to write the derived vcpkg.json / ports.txt")
    parser.add_argument(
        "--with-tests",
        action="store_true",
        help="keep the ports the test tree needs (gtest); omit for a packaging build",
    )
    args = parser.parse_args()

    skip = dict(SKIP)
    if not args.with_tests:
        skip.update(TESTS_ONLY)

    with open("vcpkg.json", encoding="utf-8") as fh:
        manifest = json.load(fh)

    # Typo guard: a SKIP entry that no longer exists in vcpkg.json is a stale rule, and silently
    # ignoring it would hide the drift that actually breaks the build.
    unknown = sorted(set(skip) - set(manifest["dependencies"]))
    if unknown:
        sys.exit("SKIP names ports absent from vcpkg.json: " + ", ".join(unknown))

    ports = [dep for dep in manifest["dependencies"] if dep not in skip]
    if not ports:
        sys.exit("every declared port is skipped -- the manifest would be empty")

    manifest["dependencies"] = ports

    os.makedirs(args.out_dir, exist_ok=True)
    # newline="\n" matters: on the Windows runner a text-mode write produces CRLF, and the stray \r
    # then rides along inside "gtest\r:x64-windows" and makes vcpkg reject the package spec.
    with open(os.path.join(args.out_dir, "vcpkg.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(manifest, fh, indent=2)
    with open(os.path.join(args.out_dir, "ports.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(ports) + "\n")

    print("vcpkg CI installs:", " ".join(ports))
    for port in ports:
        print("  -", port)
    return 0


if __name__ == "__main__":
    sys.exit(main())