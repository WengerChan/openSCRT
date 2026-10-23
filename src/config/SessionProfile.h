// Session profiles and the options-inheritance tree (P1-T12).
//
// The roadmap's "三级配置继承树" is the part of Phase 1 with real logic in it: a session
// only stores the fields it overrides, and anything it leaves empty is inherited from its
// nearest ancestor, falling back to the Default Session template. Editing that template and
// applying it to every existing session is then a matter of clearing the fields the
// sessions do not explicitly override.
//
// Keeping this free of Qt is deliberate -- the inheritance rules are the part worth testing,
// and they should not need a display to be verified.

#ifndef OPENCRT_CONFIG_SESSION_PROFILE_H_
#define OPENCRT_CONFIG_SESSION_PROFILE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace openscrt::config {

enum class Protocol : std::uint8_t {
    kSsh2,
    kTelnet,
    kSerial,
    kLocalShell,
};

const char* toString(Protocol protocol) noexcept;
std::optional<Protocol> protocolFromString(const std::string& text);

// A session's own settings. An unset optional means "inherit", never "clear".
struct SessionProfile {
    std::string id;
    std::string name;
    std::string parent_id;  // Empty means a top-level entry.

    std::optional<std::string> host;
    std::optional<int> port;
    std::optional<std::string> user;
    std::optional<Protocol> protocol;
    std::optional<std::string> private_key_path;

    [[nodiscard]] bool inheritsEverything() const noexcept {
        return !host && !port && !user && !protocol && !private_key_path;
    }
};

class SessionTree {
 public:
    // A profile with every field filled in, used as the root of every inheritance chain.
    SessionTree();
    explicit SessionTree(SessionProfile default_session);

    void setDefaultSession(SessionProfile profile);
    [[nodiscard]] const SessionProfile& defaultSession() const noexcept { return default_session_; }

    // Rejects an empty id, a duplicate id, and a parent that does not exist, because a cycle
    // or a dangling parent turns resolution into an infinite walk.
    bool add(SessionProfile profile, std::string* error = nullptr);
    bool remove(const std::string& id);
    [[nodiscard]] const SessionProfile* find(const std::string& id) const;

    [[nodiscard]] std::vector<const SessionProfile*> childrenOf(const std::string& parent_id) const;
    [[nodiscard]] std::size_t size() const noexcept { return profiles_.size(); }
    [[nodiscard]] const std::vector<SessionProfile>& profiles() const noexcept { return profiles_; }

    // Effective settings for a session after walking up the inheritance chain.
    struct Resolved {
        std::string host;
        int port{22};
        std::string user;
        Protocol protocol{Protocol::kSsh2};
        std::string private_key_path;
    };

    // Returns false when the id is unknown or the chain does not terminate. The chain always
    // terminates at the Default Session, which is fully populated.
    [[nodiscard]] bool resolve(const std::string& id, Resolved& out) const;

    // The effective settings of `id`, flattened into a profile. A transport needs plain
    // values, not a chain to walk, and the caller should not have to resolve the tree twice.
    // Returns an empty profile when the id is unknown.
    [[nodiscard]] SessionProfile effectiveProfile(const std::string& id) const;

    // Clears every explicitly overridden field in `id` and its descendants, so they all follow
    // the Default Session template again. Fields the sessions set on purpose are the caller's
    // to restore.
    void inheritEverythingBelow(const std::string& id);

 private:
    [[nodiscard]] bool chainTerminates(const std::string& id) const;

    SessionProfile default_session_;
    std::vector<SessionProfile> profiles_;
};

}  // namespace openscrt::config

#endif  // OPENCRT_CONFIG_SESSION_PROFILE_H_