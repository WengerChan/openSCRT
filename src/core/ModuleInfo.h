#ifndef OPENCRT_CORE_MODULE_INFO_H_
#define OPENCRT_CORE_MODULE_INFO_H_

namespace openscrt::core {

// Diagnostics surface: the About box and `openscrt --version` report every linked module so
// bug reports can be matched against an exact build.
[[nodiscard]] const char* moduleName() noexcept;
[[nodiscard]] const char* moduleVersion() noexcept;

}  // namespace openscrt::core

#endif  // OPENCRT_CORE_MODULE_INFO_H_