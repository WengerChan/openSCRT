// The SecureCRT session importer (P2-T11).
//
// SecureCRT keeps its sessions in a `Sessions` directory: one file per session, and the
// folders of the Session Manager are directories of that tree. This reads that tree and
// turns it into a session tree of our own.
//
// The format is not the "[Connection]" INI that SecureCRT is often described as. A
// session file is a flat list of typed values:
//
//     S:"Hostname"=172.10.3.2          a string
//     D:"[SSH2] Port"=00000016         a number, eight hex digits
//     B:"Normal Font v2"=00000060      bytes, spread over several lines
//
// and the tree is the directories: every folder has a `__FolderData__.ini` that lists
// its sessions and its subfolders, separated by colons.
//
// The passwords are not read. SecureCRT encrypts them with a key that belongs to the
// installation that wrote them, so there is nothing to import; the session comes in
// without one and the user is asked for it when it opens.

#ifndef OPENCRT_CONFIG_IMPORTERS_SECURECRT_CONFIG_IMPORTER_H_
#define OPENCRT_CONFIG_IMPORTERS_SECURECRT_CONFIG_IMPORTER_H_

#include <string>
#include <vector>

#include "config/SessionProfile.h"

namespace openscrt::config {

// A session that was found but not imported, and why.
struct SecureCrtSkipped {
    std::string name;
    std::string reason;
};

struct SecureCrtImportResult {
    bool ok{false};
    std::string error;
    // The imported sessions. The parents come before their children, so they can be
    // added to a SessionTree one after another.
    std::vector<SessionProfile> sessions;
    // What was found but not imported.
    std::vector<SecureCrtSkipped> skipped;
    // What is worth telling the user about the sessions that did come over.
    std::vector<std::string> notes;
};

class SecureCrtConfigImporter {
 public:
    // Imports a whole Sessions directory, with the folders below it.
    static SecureCrtImportResult importTree(const std::string& sessions_root);

    // Imports a single session file. The name of the session is the name of the file
    // without its extension.
    static SecureCrtImportResult importSession(const std::string& ini_path);
};

}  // namespace openscrt::config

#endif  // OPENCRT_CONFIG_IMPORTERS_SECURECRT_CONFIG_IMPORTER_H_
