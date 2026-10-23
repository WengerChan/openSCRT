# openSCRT

A SecureCRT-alike terminal emulator written in C++20 and Qt6.

## Architecture

[`securecrt_native_architecture_and_roadmap.md`](securecrt_native_architecture_and_roadmap.md) is the
**source of truth** for the design. This README only points at it; it never restates it.

## Development Plan

Milestones and task breakdown live in [`docs/DEVELOPMENT_PLAN.md`](docs/DEVELOPMENT_PLAN.md).

## System requirements

| Platform | Requirement |
| --- | --- |
| Windows | **Windows 10 1809 (build 17763) or later.** The Qt runtime in the package imports kernel32 exports that older systems do not export, so the program does not start below that — the loader says so before `main()`, with no application message. The floor is measured from the built binaries' import tables by a CI gate, not taken from a document. |
| macOS | macOS 12 or later (Qt 6.8 requirement) |
| Linux | glibc 2.34 or later, a Wayland or X11 session |

The 0.1.0 and 0.1.1 releases are **command-line programs with no window**: running the binary
prints its version and exits. **0.1.2 is the first release with a window** — it carries the Qt
workspace shell (P1-T12) and the working local shell channel.

## Installing

Releases are **tag-only**: pushes to `main` run tests, a `v*` tag additionally builds the Windows
installer and publishes it. The tag must match `VERSION` in [`CMakeLists.txt`](CMakeLists.txt), so
bump the version first, then tag.

```
git tag v0.1.0 && git push origin v0.1.0
```

| Asset | Install |
| --- | --- |
| `openSCRT-<version>-windows-x64.msi` | Double-click. Per-machine install to `%ProgramFiles%\openSCRT` with an uninstall entry in Add/Remove Programs. The installer does not touch `PATH` — add `%ProgramFiles%\openSCRT\bin` yourself if you want the command on `PATH`. |
| `openSCRT-<version>-windows-x64-portable.zip` | Unzip anywhere, run `bin\openscrt.exe`. No installer, no admin rights. |

Both bundle every DLL the binary imports (`libsodium`, the UCRT forwarders, and the Qt runtime
the window needs), so neither needs Qt installed. Verify a download against the
`SHA256SUMS.txt` attached to the release. Packaging lives in
[`.github/workflows/release.yml`](.github/workflows/release.yml) and verifies itself before
uploading: the staged exe must report its version, and the MSI is unpacked with `msiexec /a` and
the extracted binary is run.

**0.1.0 and 0.1.1 are the Phase 0 CLI bootstrap** — `openscrt --version` prints the linked
module versions. There is no window in them, so double-clicking `openscrt.exe` shows a short
message explaining that instead of a console that flashes and closes. **0.1.2 opens the window**:
start it, then `File → New Session (Quick Connect)...` to reach a server, or open a local shell.

## Building

Local builds are **not** the verification path — GitHub Actions is. A change is considered
verified when `build-test (ubuntu-24.04)`, `build-test (windows-2022)` and `build-test (macos-14)`
are green and `format (ubuntu-24.04)` passes. Use a local build only for fast iteration; never
treat it as a substitute for CI.

Cold start to green is budgeted at ≤45 min (DEVELOPMENT_PLAN P0-A6) and every job enforces a hard
timeout, so a declared-but-unlinked dependency that triggers a source build trips it instead of
quietly costing an hour.

![CI](https://github.com/<owner>/openSCRT/actions/workflows/ci.yml/badge.svg)

Local commands, if you want them:

```
vcpkg install                       # installs vcpkg.json into vcpkg_installed/
cmake --preset linux-ci             # after copying cmake/CMakePresets.json to the repo root
cmake --build --preset linux-ci
ctest --preset linux-ci             # or: ctest -L perf for the benchmark tier
```

## Prerequisites

| Tool | Version | Notes |
| --- | --- | --- |
| CMake | >= 3.28 | 3.28 is the project minimum (`cmake_minimum_required(VERSION 3.28)`). |
| Ninja | any recent | Presets use `Ninja` / `Ninja Multi-Config`. |
| C++ compiler | C++20 | GCC 13+, Clang 16+, or MSVC 19.35+ (v142 / VS 2022). |
| vcpkg | baseline in `vcpkg.json` | Manifest mode; `builtin-baseline` pins the registry. |
| Qt | 6.8.x | `Core`, `Gui`, `Widgets`, `SerialPort`. In CI Qt comes from aqt, not vcpkg. |
| Git | any | Only needed for vcpkg and CI. |

Warnings are errors by default in CI (`OPENCRT_WARNINGS_AS_ERRORS=ON`), so build clean.

## Directory Layout

```
.
├── CMakeLists.txt            # top-level build; defines the targets below
├── vcpkg.json                # pinned dependency manifest
├── cmake/CMakePresets.json   # CI presets: linux-ci, windows-ci, macos-ci
├── src/
│   ├── app/                  # executable target: openscrt
│   ├── core/                 # openscrt_core       - primitives, logging, buffers
│   ├── terminal/             # openscrt_terminal   - sessions, VT, rendering
│   ├── protocols/            # openscrt_protocols  - SSH, serial, telnet
│   ├── transfer/             # openscrt_transfer   - SFTP/SCP/X/YMODEM/ZMODEM
│   ├── credential/           # openscrt_credential - vault, agent forwarding
│   ├── scripting/            # openscrt_scripting  - scripting engine (pybind11)
│   ├── config/               # openscrt_config     - profiles, session registry
│   └── ui/                   # openscrt_ui         - Qt widgets
├── tests/                    # CTest targets, labels: unit, perf
└── docs/                     # design notes, DEVELOPMENT_PLAN.md
```

Library targets, in link order: `openscrt_core`, `openscrt_terminal`, `openscrt_protocols`,
`openscrt_transfer`, `openscrt_credential`, `openscrt_scripting`, `openscrt_config`,
`openscrt_ui`; plus the `openscrt` executable.

CMake cache switches: `OPENCRT_BUILD_TESTS` (ON by default) and `OPENCRT_WARNINGS_AS_ERRORS`.

## License

To be decided. Until a license is chosen the project is **all rights reserved** — do not
redistribute.