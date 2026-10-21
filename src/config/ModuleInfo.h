#ifndef OPENCRT_CONFIG_MODULE_INFO_H_
#define OPENCRT_CONFIG_MODULE_INFO_H_

namespace openscrt::config {

[[nodiscard]] const char* moduleName() noexcept;
[[nodiscard]] const char* moduleVersion() noexcept;

}  // namespace openscrt::config

#endif  // OPENCRT_CONFIG_MODULE_INFO_H_
