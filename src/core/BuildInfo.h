// Build identity.
//
// OPENCRT_VERSION is injected by CMake from PROJECT_VERSION for every target that links
// openscrt_core PUBLIC, so a bug report's `--version` output always matches the binary that
// produced it. The fallback keeps an IDE that configures without CMake from breaking.

#ifndef OPENCRT_CORE_BUILD_INFO_H_
#define OPENCRT_CORE_BUILD_INFO_H_

#ifndef OPENCRT_VERSION
#define OPENCRT_VERSION "0.0.0-dev"
#endif

namespace openscrt::core {

inline constexpr const char* kProjectVersion = OPENCRT_VERSION;

}  // namespace openscrt::core

#endif  // OPENCRT_CORE_BUILD_INFO_H_