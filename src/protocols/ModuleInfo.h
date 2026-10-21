#ifndef OPENCRT_PROTOCOLS_MODULE_INFO_H_
#define OPENCRT_PROTOCOLS_MODULE_INFO_H_

namespace openscrt::protocols {

[[nodiscard]] const char* moduleName() noexcept;
[[nodiscard]] const char* moduleVersion() noexcept;

}  // namespace openscrt::protocols

#endif  // OPENCRT_PROTOCOLS_MODULE_INFO_H_
