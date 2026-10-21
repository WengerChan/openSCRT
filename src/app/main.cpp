#include "config/ModuleInfo.h"
#include "core/BuildInfo.h"
#include "core/ModuleInfo.h"
#include "core/SecureByteArray.h"
#include "credential/ModuleInfo.h"
#include "protocols/ITransportChannel.h"
#include "protocols/ModuleInfo.h"
#include "scripting/ModuleInfo.h"
#include "terminal/ModuleInfo.h"
#include "transfer/ModuleInfo.h"
#include "ui/ModuleInfo.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void printVersion() {
    std::printf("openscrt %s\n", openscrt::core::kProjectVersion);
    std::printf("  %-20s %s\n", openscrt::core::moduleName(), openscrt::core::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::terminal::moduleName(),
                openscrt::terminal::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::protocols::moduleName(),
                openscrt::protocols::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::transfer::moduleName(),
                openscrt::transfer::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::credential::moduleName(),
                openscrt::credential::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::scripting::moduleName(),
                openscrt::scripting::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::config::moduleName(), openscrt::config::moduleVersion());
    std::printf("  %-20s %s\n", openscrt::ui::moduleName(), openscrt::ui::moduleVersion());

    // Report the memory-protection degradation state explicitly: P2-A7 requires the user to
    // be told when mlock could not be acquired instead of silently running unprotected.
    openscrt::core::SecureByteArray probe;
    probe.resize(1);
    if (!probe.locked()) {
        std::printf("  note: mlock unavailable (RLIMIT_MEMLOCK); credential buffers will be "
                    "zeroed but not pinned\n");
    }
    probe.wipe();
}

}  // namespace

int main(int argc, char** argv) {
    // Command line parsing compatible with SecureCRT's /S, /SSH2, /SCRIPT switches lands in
    // P1-T12 together with the session manager.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            printVersion();
            return 0;
        }
    }

    std::printf("openscrt %s - terminal emulator bootstrap (Phase 0)\n", openscrt::core::kProjectVersion);
    std::printf("The Qt workspace shell arrives in P1-T12; run with --version for build details.\n");
    return 0;
}