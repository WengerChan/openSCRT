#ifndef OPENCRT_SCRIPTING_MODULE_INFO_H_
#define OPENCRT_SCRIPTING_MODULE_INFO_H_

namespace openscrt::scripting {

[[nodiscard]] const char* moduleName() noexcept;
[[nodiscard]] const char* moduleVersion() noexcept;

}  // namespace openscrt::scripting

#endif  // OPENCRT_SCRIPTING_MODULE_INFO_H_
