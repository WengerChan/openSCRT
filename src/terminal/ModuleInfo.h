#ifndef OPENCRT_TERMINAL_MODULE_INFO_H_
#define OPENCRT_TERMINAL_MODULE_INFO_H_

namespace openscrt::terminal {

[[nodiscard]] const char* moduleName() noexcept;
[[nodiscard]] const char* moduleVersion() noexcept;

}  // namespace openscrt::terminal

#endif  // OPENCRT_TERMINAL_MODULE_INFO_H_
