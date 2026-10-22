#include "scripting/ModuleInfo.h"

#include "core/BuildInfo.h"

namespace openscrt::scripting {

const char* moduleName() noexcept {
    return "openscrt_scripting";
}

const char* moduleVersion() noexcept {
    // Phase 0 placeholder: modules have no version of their own yet, so they report the
    // project version rather than a second number that drifts from the installer.
    return openscrt::core::kProjectVersion;
}

}  // namespace openscrt::scripting
