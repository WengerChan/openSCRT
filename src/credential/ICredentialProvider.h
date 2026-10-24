// The credential provider abstraction (P2-T1).
//
// Two providers implement it: the local .kdbx reader and the KeePassHTTP client. The
// session layer above them asks for a credential and never learns where it came from.
// That separation is the point of the interface: the alternative -- a session that knows
// about KeePass -- puts a password store inside the code that opens TCP connections, and
// every path that can reach a password becomes a path that can leak one.
//
// What the interface encodes, and why:
//
//   * The plaintext lives in a locked buffer (core::SecureByteArray) and nowhere else.
//     There is no std::string path on the way out, so a password cannot end up in a log
//     line or in a QString by accident.
//   * A provider never writes the plaintext anywhere: not into a session profile, not
//     into the INI, not into an unencrypted keychain entry.
//   * A lookup that cannot be answered says so. An empty password returned as "found"
//     would be indistinguishable from a wrong password, and the user would go debug the
//     wrong half of the problem.
//   * lookups block. Argon2 with a real KeePass parameter set takes 300-1500 ms, so the
//     caller runs it on a worker thread and the UI shows a progress indicator (P2-A4).
//     The interface says so, so nobody calls it from the GUI thread by accident.

#ifndef OPENCRT_CREDENTIAL_I_CREDENTIAL_PROVIDER_H_
#define OPENCRT_CREDENTIAL_I_CREDENTIAL_PROVIDER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/SecureByteArray.h"

namespace openscrt::credential {

// Which value of a KeePass entry a session is asking for. The roadmap asks for the SSH
// private key passphrase, the jump host password, the sudo password and the TOTP code as
// well as the login password, and they come out of the same entry.
enum class CredentialField : std::uint8_t {
    kPassword,
    kPrivateKeyPassphrase,
    kJumpHostPassword,
    kSudoPassword,
    kTotp,
};

[[nodiscard]] const char* toString(CredentialField field) noexcept;

// How a session decides where its password comes from. This is session configuration, so
// it is plain data that the profile can persist -- the provider never sees it, the
// session layer resolves it and then calls a provider.
enum class CredentialSource : std::uint8_t {
    kSessionSaved,      // the password stored with the session
    kPromptEveryTime,   // ask the user at connect time
    kKeePassUuid,       // a KeePass entry bound by UUID
    kKeePassAutoMatch,  // a KeePass entry matched by host / title
};

[[nodiscard]] const char* toString(CredentialSource source) noexcept;
// Takes a string_view rather than a const std::string&: a caller that passes a literal
// would otherwise build a temporary std::string inside a noexcept function, and the
// allocation could throw -- which would turn a bad_alloc into a terminate.
[[nodiscard]] std::optional<CredentialSource> credentialSourceFromString(
    std::string_view text) noexcept;

// What the session asks for. Everything here is non-secret: the host and the user name
// are the lookup key, and the request never carries the password it wants back.
struct CredentialRequest {
    std::string host;
    int port{22};
    std::string user;
    // The session's own title, used by the auto-match strategy when no host matches.
    std::string title;
    // Set when the session binds one specific KeePass entry.
    std::string entry_uuid;
    CredentialField field{CredentialField::kPassword};
};

// One KeePass entry, flattened. The password stays in locked memory; the custom fields
// are plain text because the KeePass format defines them as text.
struct CredentialEntry {
    std::string uuid;
    std::string name;
    std::string login;
    core::SecureByteArray password;
    std::vector<std::pair<std::string, std::string>> string_fields;
};

enum class CredentialStatus : std::uint8_t {
    kFound,
    kNotFound,     // the provider answered: no entry matches
    kCancelled,    // the user dismissed the picker
    kUnavailable,  // the provider cannot be reached at all (KeePass is not running)
    kError,        // the provider failed (wrong master password, corrupt file, ...)
};

[[nodiscard]] const char* toString(CredentialStatus status) noexcept;

struct CredentialResult {
    CredentialStatus status{CredentialStatus::kNotFound};
    CredentialEntry entry;
    // Human-readable detail for the error path. Never contains a secret: the providers
    // are responsible for saying what went wrong without saying what the secret was.
    std::string message;
};

// Moves the plaintext out of `result` into `out` and wipes it from the result.
//
// The result is the only other holder of those bytes, and the caller's buffer is the one
// that outlives the call, so the transfer has to leave the result empty. Returns false
// when there was nothing to take -- a failed lookup, or a field the entry does not have.
[[nodiscard]] bool takeSecret(CredentialResult& result, core::SecureByteArray& out);

// The interface every credential source implements.
class ICredentialProvider {
 public:
    virtual ~ICredentialProvider() = default;

    // A stable identifier for the provider ("kdbx", "keepasshttp"), used in logs and in
    // the "where did this password come from" answer in the UI.
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    // Whether the provider can answer right now. A KeePassHTTP provider that cannot
    // reach 127.0.0.1:19455 is unavailable; a .kdbx provider whose file is gone is too.
    [[nodiscard]] virtual bool isAvailable() const noexcept = 0;

    // Looks the request up. Blocks: call it on a worker thread.
    [[nodiscard]] virtual CredentialResult lookup(const CredentialRequest& request) = 0;

    // Drops any cached key material and forgets what was unlocked. Providers with
    // nothing to clear do nothing. The auto-lock timer (P2-T7) calls this, so it has to
    // be part of the interface rather than a cast to a concrete type.
    virtual void lock() noexcept {}
};

}  // namespace openscrt::credential

#endif  // OPENCRT_CREDENTIAL_I_CREDENTIAL_PROVIDER_H_
