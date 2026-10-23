#include "config/SessionProfile.h"

#include <algorithm>
#include <unordered_set>

namespace openscrt::config {

const char* toString(Protocol protocol) noexcept {
    switch (protocol) {
        case Protocol::kSsh2:
            return "SSH2";
        case Protocol::kTelnet:
            return "Telnet";
        case Protocol::kSerial:
            return "Serial";
        case Protocol::kLocalShell:
            return "Local";
    }
    return "SSH2";
}

std::optional<Protocol> protocolFromString(const std::string& text) {
    if (text == "SSH2") {
        return Protocol::kSsh2;
    }
    if (text == "Telnet") {
        return Protocol::kTelnet;
    }
    if (text == "Serial") {
        return Protocol::kSerial;
    }
    if (text == "Local") {
        return Protocol::kLocalShell;
    }
    return std::nullopt;
}

namespace {

SessionProfile makeDefaultSession() {
    SessionProfile profile;
    profile.id = "";
    profile.name = "Default Session";
    profile.host = std::string("localhost");
    profile.port = 22;
    profile.user = std::string("anonymous");
    profile.protocol = Protocol::kSsh2;
    return profile;
}

}  // namespace

SessionTree::SessionTree() : default_session_(makeDefaultSession()) {}

SessionTree::SessionTree(SessionProfile default_session)
    : default_session_(std::move(default_session)) {
    // A template with gaps would make resolution depend on which field was consulted, so a
    // Default Session is required to be complete.
    if (!default_session_.host) {
        default_session_.host = std::string("localhost");
    }
    if (!default_session_.port) {
        default_session_.port = 22;
    }
    if (!default_session_.user) {
        default_session_.user = std::string("anonymous");
    }
    if (!default_session_.protocol) {
        default_session_.protocol = Protocol::kSsh2;
    }
}

void SessionTree::setDefaultSession(SessionProfile profile) {
    SessionTree complete{std::move(profile)};
    default_session_ = std::move(complete.default_session_);
}

bool SessionTree::add(SessionProfile profile, std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (profile.id.empty()) {
        return fail("session id must not be empty");
    }
    if (find(profile.id) != nullptr) {
        return fail("duplicate session id: " + profile.id);
    }
    if (!profile.parent_id.empty() && find(profile.parent_id) == nullptr) {
        return fail("unknown parent: " + profile.parent_id);
    }
    // Adding under a descendant would close a cycle.
    if (!profile.parent_id.empty()) {
        std::string cursor = profile.parent_id;
        while (!cursor.empty()) {
            if (cursor == profile.id) {
                return fail("parent chain would form a cycle at " + profile.id);
            }
            const SessionProfile* node = find(cursor);
            cursor = (node == nullptr) ? std::string{} : node->parent_id;
        }
    }
    profiles_.push_back(std::move(profile));
    return true;
}

bool SessionTree::remove(const std::string& id) {
    // Children would be orphaned, so refuse rather than silently re-rooting them.
    if (!childrenOf(id).empty()) {
        return false;
    }
    const auto found =
        std::remove_if(profiles_.begin(), profiles_.end(),
                       [&id](const SessionProfile& profile) { return profile.id == id; });
    if (found == profiles_.end()) {
        return false;
    }
    // remove_if only compacts. Without erase the removed profile still occupies a slot, so
    // childrenOf() keeps matching it and the parent could never be deleted afterwards.
    profiles_.erase(found, profiles_.end());
    return true;
}

const SessionProfile* SessionTree::find(const std::string& id) const {
    const auto found =
        std::find_if(profiles_.begin(), profiles_.end(),
                     [&id](const SessionProfile& profile) { return profile.id == id; });
    return found == profiles_.end() ? nullptr : &*found;
}

std::vector<const SessionProfile*> SessionTree::childrenOf(const std::string& parent_id) const {
    std::vector<const SessionProfile*> children;
    for (const SessionProfile& profile : profiles_) {
        if (profile.parent_id == parent_id) {
            children.push_back(&profile);
        }
    }
    return children;
}

bool SessionTree::resolve(const std::string& id, Resolved& out) const {
    const SessionProfile* start = find(id);
    if (start == nullptr) {
        return false;
    }

    out.host = default_session_.host.value_or("");
    out.port = default_session_.port.value_or(22);
    out.user = default_session_.user.value_or("");
    out.protocol = default_session_.protocol.value_or(Protocol::kSsh2);
    out.private_key_path = default_session_.private_key_path.value_or("");

    // Walk from the session up to the root. The NEAREST value has to win, so each field is
    // filled only once: writing unconditionally lets the most distant ancestor overwrite it,
    // because the walk ends at the root.
    bool have_host = !out.host.empty();
    bool have_user = !out.user.empty();
    bool have_key = !out.private_key_path.empty();
    bool have_port = false;  // 0 is not a valid port, so "unset" has to be tracked separately.
    bool have_protocol = false;

    std::unordered_set<std::string> seen;
    const SessionProfile* node = start;
    while (node != nullptr) {
        if (!seen.insert(node->id).second) {
            return false;  // Cycle; should be impossible, but never trust a loaded file.
        }
        if (!have_host && node->host) {
            out.host = *node->host;
            have_host = true;
        }
        if (!have_port && node->port) {
            out.port = *node->port;
            have_port = true;
        }
        if (!have_user && node->user) {
            out.user = *node->user;
            have_user = true;
        }
        if (!have_protocol && node->protocol) {
            out.protocol = *node->protocol;
            have_protocol = true;
        }
        if (!have_key && node->private_key_path) {
            out.private_key_path = *node->private_key_path;
            have_key = true;
        }
        node = node->parent_id.empty() ? nullptr : find(node->parent_id);
    }
    return true;
}

void SessionTree::inheritEverythingBelow(const std::string& id) {
    for (SessionProfile& profile : profiles_) {
        if (profile.id == id) {
            profile.host.reset();
            profile.port.reset();
            profile.user.reset();
            profile.protocol.reset();
            profile.private_key_path.reset();
            continue;
        }
        // Only descendants, found by walking up from each candidate.
        std::string cursor = profile.parent_id;
        bool is_descendant = false;
        std::unordered_set<std::string> seen;
        while (!cursor.empty() && seen.insert(cursor).second) {
            if (cursor == id) {
                is_descendant = true;
                break;
            }
            const SessionProfile* node = find(cursor);
            cursor = (node == nullptr) ? std::string{} : node->parent_id;
        }
        if (!is_descendant) {
            continue;
        }
        profile.host.reset();
        profile.port.reset();
        profile.user.reset();
        profile.protocol.reset();
        profile.private_key_path.reset();
    }
}

}  // namespace openscrt::config