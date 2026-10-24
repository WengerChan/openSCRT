#include "config/importers/SecureCrtConfigImporter.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace openscrt::config {
namespace {

namespace fs = std::filesystem;

// The file in a folder that says what is in it.
constexpr const char* kFolderData = "__FolderData__.ini";

// One value of a session file.
struct SecureCrtValue {
    // 'S' for a string, 'D' for a number, 'B' for bytes.
    char type{'S'};
    std::string key;
    std::string value;
};

// Parses one line of a session file.
//
// The shape is fixed: a type letter, a colon, the key in quotes, an equals sign and
// the value. A line that does not have that shape is not a value and is skipped.
std::optional<SecureCrtValue> parseValue(const std::string& line) {
    if (line.size() < 5) {
        return std::nullopt;
    }
    const char type = line[0];
    if (type != 'S' && type != 'D' && type != 'B' && type != 'Z') {
        return std::nullopt;
    }
    if (line[1] != ':' || line[2] != '"') {
        return std::nullopt;
    }
    const std::size_t quote = line.find('"', 3);
    if (quote == std::string::npos || quote + 1 >= line.size() || line[quote + 1] != '=') {
        return std::nullopt;
    }
    SecureCrtValue value;
    value.type = type;
    value.key = line.substr(3, quote - 3);
    value.value = line.substr(quote + 2);
    return value;
}

// The values of a session file, in the order they were written.
std::vector<SecureCrtValue> readValues(const std::string& path, bool& readable) {
    std::vector<SecureCrtValue> values;
    readable = false;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return values;
    }
    readable = true;
    std::string line;
    bool first = true;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (first) {
            first = false;
            // The file starts with a byte order mark; it is not part of the first key.
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }
        const std::optional<SecureCrtValue> value = parseValue(line);
        if (value.has_value()) {
            values.push_back(*value);
        }
    }
    return values;
}

const SecureCrtValue* findValue(const std::vector<SecureCrtValue>& values, const std::string& key) {
    for (const SecureCrtValue& value : values) {
        if (value.key == key) {
            return &value;
        }
    }
    return nullptr;
}

// The value of a string entry, or an empty string.
std::string stringValue(const std::vector<SecureCrtValue>& values, const std::string& key) {
    const SecureCrtValue* value = findValue(values, key);
    if (value == nullptr) {
        return {};
    }
    return value->value;
}

// The value of a number entry.
//
// SecureCRT writes numbers as eight hex digits, so a value that is not eight hex
// digits is not a number this build can read. A value of "22" would read as 34 in hex,
// which is worse than not reading it at all.
std::optional<int> numberValue(const std::vector<SecureCrtValue>& values, const std::string& key) {
    const SecureCrtValue* value = findValue(values, key);
    if (value == nullptr || value->type != 'D') {
        return std::nullopt;
    }
    const std::string& text = value->value;
    if (text.size() != 8) {
        return std::nullopt;
    }
    unsigned long parsed = 0;
    for (const char c : text) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A') + 10;
        } else {
            return std::nullopt;
        }
        parsed = parsed * 16 + digit;
    }
    if (parsed > 65535) {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

// The protocol of a session, and the key its port is stored under.
struct ProtocolChoice {
    Protocol protocol{Protocol::kSsh2};
    const char* port_key{"[SSH2] Port"};
    int default_port{22};
};

std::optional<ProtocolChoice> chooseProtocol(const std::string& name) {
    std::string upper;
    upper.reserve(name.size());
    for (const char c : name) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (upper == "SSH2") {
        return ProtocolChoice{Protocol::kSsh2, "[SSH2] Port", 22};
    }
    if (upper == "SSH1") {
        // SecureCRT still writes SSH1 sessions. The transport of this project speaks
        // SSH2, which every server that is still around speaks as well.
        return ProtocolChoice{Protocol::kSsh2, "[SSH1] Port", 22};
    }
    if (upper == "TELNET") {
        return ProtocolChoice{Protocol::kTelnet, "Port", 23};
    }
    return std::nullopt;
}

// The entries of a colon separated list, as SecureCRT writes them.
std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : text) {
        if (c == ':') {
            if (!current.empty()) {
                parts.push_back(current);
            }
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

// The name of a session file without its extension.
std::string fileStem(const std::string& name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) {
        return name;
    }
    return name.substr(0, dot);
}

// The id of a session or a folder inside the imported tree.
//
// The id is the path from the root, with the names joined by a dot. A dot and not a
// slash, because the id becomes a group name in the session file, and a slash there
// would make it a group of groups.
std::string joinId(const std::string& prefix, const std::string& name) {
    if (prefix.empty()) {
        return name;
    }
    return prefix + "." + name;
}

// Reads one session file into a profile. Returns false when the file is not a session
// this build can bring over.
bool buildProfile(const std::string& path, const std::string& id, const std::string& name,
                  SessionProfile& profile, bool& has_password, std::string& reason) {
    bool readable = false;
    const std::vector<SecureCrtValue> values = readValues(path, readable);
    if (!readable) {
        reason = "the file could not be read";
        return false;
    }

    has_password = !stringValue(values, "Password V2").empty();

    const std::string protocol_name = stringValue(values, "Protocol Name");
    const std::optional<ProtocolChoice> choice = chooseProtocol(protocol_name);
    if (!choice.has_value()) {
        reason = protocol_name.empty() ? "the session has no protocol"
                                       : "the protocol " + protocol_name + " is not supported yet";
        return false;
    }

    const std::string host = stringValue(values, "Hostname");
    if (host.empty()) {
        reason = "the session has no host";
        return false;
    }

    profile.id = id;
    profile.name = name;
    profile.host = host;
    profile.protocol = choice->protocol;
    profile.port = numberValue(values, choice->port_key).value_or(choice->default_port);

    const std::string user = stringValue(values, "Username");
    if (!user.empty()) {
        profile.user = user;
    }
    const std::string key = stringValue(values, "Identity Filename V2");
    if (!key.empty()) {
        profile.private_key_path = key;
    }
    return true;
}

// The folder that holds the sessions, and the folders below it.
struct FolderListing {
    std::vector<std::string> sessions;
    std::vector<std::string> folders;
};

FolderListing readFolder(const fs::path& directory) {
    FolderListing listing;
    const fs::path data = directory / kFolderData;
    if (fs::exists(data)) {
        bool readable = false;
        const std::vector<SecureCrtValue> values = readValues(data.string(), readable);
        if (readable) {
            listing.sessions = splitList(stringValue(values, "Session List"));
            listing.folders = splitList(stringValue(values, "Folder List"));
        }
    }

    // Whatever the listing does not mention is still there, so the directory is walked
    // as well: a listing that is one session short should not lose that session.
    std::vector<std::string> extra_sessions;
    std::vector<std::string> extra_folders;
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        const std::string file_name = entry.path().filename().string();
        if (entry.is_directory()) {
            if (std::find(listing.folders.begin(), listing.folders.end(), file_name) ==
                listing.folders.end()) {
                extra_folders.push_back(file_name);
            }
            continue;
        }
        if (file_name == kFolderData || entry.path().extension() != ".ini") {
            continue;
        }
        const std::string stem = fileStem(file_name);
        if (std::find(listing.sessions.begin(), listing.sessions.end(), stem) ==
            listing.sessions.end()) {
            extra_sessions.push_back(stem);
        }
    }
    std::sort(extra_sessions.begin(), extra_sessions.end());
    std::sort(extra_folders.begin(), extra_folders.end());
    listing.sessions.insert(listing.sessions.end(), extra_sessions.begin(), extra_sessions.end());
    listing.folders.insert(listing.folders.end(), extra_folders.begin(), extra_folders.end());
    return listing;
}

// Imports a directory: its sessions and the folders below it.
void importDirectory(const fs::path& directory, const std::string& id_prefix,
                     const std::string& parent_id, SecureCrtImportResult& result) {
    const FolderListing listing = readFolder(directory);

    // The sessions of this folder come first, then the folders below it. A session of a
    // folder is imported before the folders, so the tree reads the way the Session
    // Manager shows it.
    for (const std::string& session : listing.sessions) {
        const fs::path file = directory / (session + ".ini");
        std::error_code error;
        if (!fs::exists(file, error)) {
            result.skipped.push_back(SecureCrtSkipped{session, "the file is not there"});
            continue;
        }
        const std::string session_id = joinId(id_prefix, session);
        SessionProfile profile;
        bool has_password = false;
        std::string reason;
        if (!buildProfile(file.string(), session_id, session, profile, has_password, reason)) {
            result.skipped.push_back(SecureCrtSkipped{session, reason});
            continue;
        }
        profile.parent_id = parent_id;
        result.sessions.push_back(std::move(profile));

        // A password that SecureCRT encrypted cannot be brought over. Saying so is
        // better than a session that quietly asks for a password it should have had.
        if (has_password) {
            result.notes.push_back(session +
                                   ": the password is encrypted by SecureCRT and was not "
                                   "imported");
        }
    }

    for (const std::string& folder : listing.folders) {
        const fs::path child = directory / folder;
        std::error_code error;
        if (!fs::is_directory(child, error)) {
            continue;
        }
        const std::string folder_id = joinId(id_prefix, folder);
        SessionProfile group;
        group.id = folder_id;
        group.name = folder;
        group.parent_id = parent_id;
        result.sessions.push_back(std::move(group));

        importDirectory(child, folder_id, folder_id, result);
    }
}

}  // namespace

SecureCrtImportResult SecureCrtConfigImporter::importTree(const std::string& sessions_root) {
    SecureCrtImportResult result;
    std::error_code error;
    const fs::path root(sessions_root);
    if (!fs::is_directory(root, error)) {
        result.error = "not a directory: " + sessions_root;
        return result;
    }

    importDirectory(root, std::string{}, std::string{}, result);
    result.ok = true;
    return result;
}

SecureCrtImportResult SecureCrtConfigImporter::importSession(const std::string& ini_path) {
    SecureCrtImportResult result;
    const fs::path file(ini_path);
    std::error_code error;
    if (!fs::is_regular_file(file, error)) {
        result.error = "not a file: " + ini_path;
        return result;
    }

    const std::string name = fileStem(file.filename().string());
    SessionProfile profile;
    bool has_password = false;
    std::string reason;
    if (!buildProfile(ini_path, name, name, profile, has_password, reason)) {
        result.error = reason;
        return result;
    }
    result.sessions.push_back(std::move(profile));
    if (has_password) {
        result.notes.push_back(name +
                               ": the password is encrypted by SecureCRT and was not imported");
    }
    result.ok = true;
    return result;
}

}  // namespace openscrt::config
