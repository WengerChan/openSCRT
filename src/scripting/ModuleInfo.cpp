#include "scripting/ModuleInfo.h"

namespace openscrt::scripting {

const char* moduleName() noexcept {
    return "openscrt_scripting";
}

const char* moduleVersion() noexcept {
    return "0.1.0";
}

}  // namespace openscrt::scripting
