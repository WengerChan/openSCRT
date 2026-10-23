// Session tree persistence (P1-T12).
//
// Reads and writes the tree through QSettings' INI form. The requirement that shapes the
// format: only fields a session actually overrides are written. Saving the resolved values
// would flatten the inheritance tree on the first save, and every later edit to the Default
// Session template would stop reaching those sessions.

#ifndef OPENCRT_CONFIG_CONFIG_MANAGER_H_
#define OPENCRT_CONFIG_CONFIG_MANAGER_H_

#include <string>

#include "config/SessionProfile.h"

class QSettings;

namespace openscrt::config {

class ConfigManager {
 public:
    // Reads from an INI file. A missing or unreadable file yields a tree with the built-in
    // default template, never an exception: a corrupt config must not stop the app starting.
    static SessionTree loadTree(const std::string& ini_path);

    // Writes only the overridden fields, so a load/save cycle is lossless for inheritance.
    // Groups this manager does not own (the window geometry) are left alone.
    static bool saveTree(const SessionTree& tree, const std::string& ini_path);

    // Per-session UI state (last used tab, window geometry). Kept separate from the tree so
    // that rewriting either does not churn the other.
    static void saveWindowGeometry(QSettings& settings, const std::string& blob);
    [[nodiscard]] static std::string loadWindowGeometry(QSettings& settings);
};

}  // namespace openscrt::config

#endif  // OPENCRT_CONFIG_CONFIG_MANAGER_H_