#ifndef OPENCRT_TRANSFER_MODULE_INFO_H_
#define OPENCRT_TRANSFER_MODULE_INFO_H_

namespace openscrt::transfer {

[[nodiscard]] const char* moduleName() noexcept;
[[nodiscard]] const char* moduleVersion() noexcept;

}  // namespace openscrt::transfer

#endif  // OPENCRT_TRANSFER_MODULE_INFO_H_
