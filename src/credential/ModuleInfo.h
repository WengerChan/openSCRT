#ifndef OPENCRT_CREDENTIAL_MODULE_INFO_H_
#define OPENCRT_CREDENTIAL_MODULE_INFO_H_

namespace openscrt::credential {

[[nodiscard]] const char* moduleName() noexcept;
[[nodiscard]] const char* moduleVersion() noexcept;

}  // namespace openscrt::credential

#endif  // OPENCRT_CREDENTIAL_MODULE_INFO_H_
