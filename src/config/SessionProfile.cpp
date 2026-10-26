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

// The checks that keep resolution possible: the parent has to exist, and the parent chain
// must not close a cycle.
//
// The walk uses the tree as it is right now. For update() that means the session being
// replaced is still in the tree with its old parent, which is what makes the cycle check
// work: a session cannot be moved under one of its own descendants.
bool validateParentChain(const SessionTree& tree, const SessionProfile& profile,
                         std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (!profile.parent_id.empty() && tree.find(profile.parent_id) == nullptr) {
        return fail("unknown parent: " + profile.parent_id);
    }
    if (!profile.parent_id.empty()) {
        std::string cursor = profile.parent_id;
        while (!cursor.empty()) {
            if (cursor == profile.id) {
                return fail("parent chain would form a cycle at " + profile.id);
            }
            const SessionProfile* node = tree.find(cursor);
            cursor = (node == nullptr) ? std::string{} : node->parent_id;
        }
    }
    return true;
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
    if (!validateParentChain(*this, profile, error)) {
        return false;
    }
    profiles_.push_back(std::move(profile));
    return true;
}

bool SessionTree::update(SessionProfile profile, std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (profile.id.empty()) {
        return fail("session id must not be empty");
    }
    const auto found =
        std::find_if(profiles_.begin(), profiles_.end(),
                     [&profile](const SessionProfile& value) { return value.id == profile.id; });
    if (found == profiles_.end()) {
        return fail("no such session: " + profile.id);
    }
    if (!validateParentChain(*this, profile, error)) {
        return false;
    }
    // The slot keeps its place, so the tree the user sees does not reshuffle under the
    // cursor because a binding was added.
    *found = std::move(profile);
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

    // Collect from the nearest ancestor outwards, then fall back to the Default Session
    // template for whatever is still unset.
    //
    // The template must NOT be pre-loaded into `out`: doing so would mark every field as
    // already filled and the walk would skip them all. Equally, writing each value
    // unconditionally would let the most distant ancestor overwrite the nearest one, because
    // the walk ends at the root. Hence: start empty, fill once, then apply the template.
    Resolved collected;
    bool have_host = false;
    bool have_port = false;
    bool have_user = false;
    bool have_protocol = false;
    bool have_key = false;

    std::unordered_set<std::string> seen;
    const SessionProfile* node = start;
    while (node != nullptr) {
        if (!seen.insert(node->id).second) {
            return false;  // Cycle; should be impossible, but never trust a loaded file.
        }
        if (!have_host && node->host) {
            collected.host = *node->host;
            have_host = true;
        }
        if (!have_port && node->port) {
            collected.port = *node->port;
            have_port = true;
        }
        if (!have_user && node->user) {
            collected.user = *node->user;
            have_user = true;
        }
        if (!have_protocol && node->protocol) {
            collected.protocol = *node->protocol;
            have_protocol = true;
        }
        if (!have_key && node->private_key_path) {
            collected.private_key_path = *node->private_key_path;
            have_key = true;
        }
        node = node->parent_id.empty() ? nullptr : find(node->parent_id);
    }

    out = collected;
    if (!have_host) {
        out.host = default_session_.host.value_or("");
    }
    if (!have_port) {
        out.port = default_session_.port.value_or(22);
    }
    if (!have_user) {
        out.user = default_session_.user.value_or("");
    }
    if (!have_protocol) {
        out.protocol = default_session_.protocol.value_or(Protocol::kSsh2);
    }
    if (!have_key) {
        out.private_key_path = default_session_.private_key_path.value_or("");
    }
    return true;
}

SessionProfile SessionTree::effectiveProfile(const std::string& id) const {
    Resolved resolved;
    if (!resolve(id, resolved)) {
        return SessionProfile{};
    }
    const SessionProfile* source = find(id);
    SessionProfile profile;
    profile.id = id;
    profile.name = (source != nullptr && !source->name.empty()) ? source->name : id;
    profile.parent_id = (source != nullptr) ? source->parent_id : std::string{};
    profile.host = resolved.host;
    profile.port = resolved.port;
    profile.user = resolved.user;
    profile.protocol = resolved.protocol;
    profile.private_key_path = resolved.private_key_path;
    // The credential binding is the session's own and does not come from the chain. A
    // child session usually points at another machine, and inheriting the parent's
    // KeePass entry would send the parent's password to it.
    profile.credential_source = source != nullptr ? source->credential_source : std::nullopt;
    profile.credential_entry_uuid =
        source != nullptr ? source->credential_entry_uuid : std::nullopt;
    profile.credential_match_host =
        source != nullptr ? source->credential_match_host : std::nullopt;
    // The serial settings are the session's own for the same reason: a child session
    // usually sits on another device.
    profile.serial_port = source != nullptr ? source->serial_port : std::nullopt;
    profile.serial_baud_rate = source != nullptr ? source->serial_baud_rate : std::nullopt;
    profile.serial_data_bits = source != nullptr ? source->serial_data_bits : std::nullopt;
    profile.serial_parity = source != nullptr ? source->serial_parity : std::nullopt;
    profile.serial_stop_bits = source != nullptr ? source->serial_stop_bits : std::nullopt;
    profile.serial_flow_control = source != nullptr ? source->serial_flow_control : std::nullopt;
    return profile;
}

void SessionTree::inheritEverythingBelow(const std::string& id) {
    // The credential binding is deliberately left alone: it is not part of the
    // inheritance chain, so there is nothing for it to fall back to. Clearing it would
    // throw away a binding the user picked by hand.
    //
    // The serial settings are left alone for the same reason: they are the session's
    // own, and there is no default port to fall back to.
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