// Resolving a session's credential binding (P2-T9).
//
// A session says *where* its password comes from, and this turns that into an entry of
// the database. The four ways a session can be bound:
//
//   session saved      the password belongs to the session itself
//   prompt every time  the user is asked when the session opens
//   KeePass by uuid    one specific entry, bound by its uuid
//   KeePass by host    the entry whose host matches
//
// The resolution is deliberately free of Qt: "which entry does this session want" is
// logic, and logic that can be tested without a display. The case that needs a human --
// the prompt -- is reported as such, and the window does the asking.

#ifndef OPENCRT_CREDENTIAL_KDBX_KDBX_CREDENTIAL_RESOLVER_H_
#define OPENCRT_CREDENTIAL_KDBX_KDBX_CREDENTIAL_RESOLVER_H_

#include <cstddef>
#include <string>

#include "credential/kdbx/KdbxXmlIndex.h"

namespace openscrt::credential::kdbx {

// Where a session gets its password.
enum class KdbxBindingSource {
    kSessionSaved,
    kPromptEveryTime,
    kKeePassUuid,
    kKeePassAutoMatch,
};

[[nodiscard]] const char* toString(KdbxBindingSource source) noexcept;
[[nodiscard]] bool kdbxBindingSourceFromString(const std::string& text,
                                               KdbxBindingSource& out) noexcept;

// The binding of a session: the source, and whatever the source needs.
struct KdbxBinding {
    KdbxBindingSource source{KdbxBindingSource::kSessionSaved};
    // The entry to use, for kKeePassUuid.
    std::string entry_uuid;
    // The host to match, for kKeePassAutoMatch. Empty means "the session's own host".
    std::string match_host;
};

enum class KdbxResolutionOutcome {
    kFound,        // the index holds the entry the session wants
    kNeedsPrompt,  // the session asks the user every time
    kNotFound,     // the binding points at an entry that is not there
};

struct KdbxResolution {
    KdbxResolutionOutcome outcome{KdbxResolutionOutcome::kNotFound};
    // Where the entry is in the index, when the outcome is kFound.
    std::size_t entry_index{0};
    // The entry itself, or null.
    [[nodiscard]] const KdbxEntry* entry(const KdbxIndex& index) const {
        return outcome == KdbxResolutionOutcome::kFound ? &index.entries[entry_index] : nullptr;
    }
};

// Resolves the binding against the index.
//
// `session_host` is the host of the session that is opening; it is what the auto-match
// falls back to when the binding does not name a host of its own.
[[nodiscard]] KdbxResolution resolveKdbxBinding(const KdbxIndex& index, const KdbxBinding& binding,
                                                const std::string& session_host);

}  // namespace openscrt::credential::kdbx

#endif  // OPENCRT_CREDENTIAL_KDBX_KDBX_CREDENTIAL_RESOLVER_H_
