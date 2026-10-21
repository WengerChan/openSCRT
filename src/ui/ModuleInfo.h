#ifndef OPENCRT_UI_MODULE_INFO_H_
#define OPENCRT_UI_MODULE_INFO_H_

namespace openscrt::ui {

[[nodiscard]] const char* moduleName() noexcept;
[[nodiscard]] const char* moduleVersion() noexcept;

}  // namespace openscrt::ui

#endif  // OPENCRT_UI_MODULE_INFO_H_
