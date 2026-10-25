# Third-party notices

openSCRT itself is under the Apache License, Version 2.0 (see `LICENSE`). It links the
components below, and the release artifacts ship the license texts of the ones that are
redistributed with them.

The versions below are the ones the build resolves today. The authoritative list is
`vcpkg.json`; the versions are pinned by vcpkg's `builtin-baseline`.

## Components linked into the executable

| Component | Version | License | Where it comes from |
| --- | --- | --- | --- |
| Qt 6 (Core, Gui, Widgets, Network) | 6.8.3 | LGPL-3.0-only (or the commercial license) | the prebuilt Qt of the CI, not vcpkg |
| libsodium | 1.0.20 | ISC | vcpkg |
| OpenSSL | 3.5.2 | Apache-2.0 | vcpkg |
| libssh | see `vcpkg.json` | BSD-3-Clause | vcpkg |
| pugixml | 1.15 | MIT | vcpkg |
| nlohmann/json | 3.12.0 | MIT | vcpkg |
| zlib | 1.3.1 | Zlib | vcpkg |

## Components linked on one platform only

| Component | Version | License | Where it is used |
| --- | --- | --- | --- |
| libsecret | 0.21.4 | LGPL-2.1-or-later | the system secret store on Linux (glib, libgcrypt, libgpg-error, libffi, gettext-libintl, libiconv and dirent come with it) |

## Test-only components

| Component | Version | License | Where it is used |
| --- | --- | --- | --- |
| GoogleTest | 1.17.0 | BSD-3-Clause | the unit tests; not in the release artifacts |
| pybind11 | see `vcpkg.json` | BSD-3-Clause | reserved for the scripting engine; nothing links it yet |

## What this means for the release artifacts

- **Qt is linked dynamically.** The Windows and macOS bundles ship the Qt libraries as
  separate files, so the LGPL's conditions for a shared library apply: the library may be
  replaced by the user with a modified build. The LGPL text ships with the bundle.
- **The Apache-2.0 terms of this project are compatible with every license above.**
  OpenSSL is Apache-2.0, the ISC/MIT/BSD/Zlib terms are permissive, and the LGPL terms
  are compatible with Apache-2.0 in this direction.
- **The license texts of the vcpkg components** are copied into the bundles from
  vcpkg's own `share/<port>/copyright` files, so they are the texts of the exact
  versions that were built.
