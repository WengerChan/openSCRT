#include "credential/kdbx/KdbxCredentialResolver.h"

namespace openscrt::credential::kdbx {
namespace {

// Does the entry's url point at that host?
//
// The url of an entry is whatever the user typed in KeePass -- "router.example",
// "ssh://router.example:22", "10.0.0.1" -- so the comparison is on the host part of
// the url, and the scheme and the port are stripped first.
bool urlMatchesHost(const std::string& url, const std::string& host) {
    if (url.empty() || host.empty()) {
        return false;
    }
    std::string candidate = url;
    const std::size_t scheme = candidate.find("://");
    if (scheme != std::string::npos) {
        candidate = candidate.substr(scheme + 3);
    }
    const std::size_t slash = candidate.find('/');
    if (slash != std::string::npos) {
        candidate = candidate.substr(0, slash);
    }
    // A port is not part of the host.
    const std::size_t colon = candidate.rfind(':');
    if (colon != std::string::npos) {
        candidate = candidate.substr(0, colon);
    }
    if (candidate == host) {
        return true;
    }
    // "router.example" also matches "router.example.local" and the other way round:
    // a name that ends with the host is the same machine as far as a password is
    // concerned.
    if (candidate.size() > host.size() &&
        candidate.compare(candidate.size() - host.size(), host.size(), host) == 0 &&
        candidate[candidate.size() - host.size() - 1] == '.') {
        return true;
    }
    return host.size() > candidate.size() &&
           host.compare(host.size() - candidate.size(), candidate.size(), candidate) == 0 &&
           host[host.size() - candidate.size() - 1] == '.';
}

}  // namespace

const char* toString(KdbxBindingSource source) noexcept {
    switch (source) {
        case KdbxBindingSource::kSessionSaved:
            return "session-saved";
        case KdbxBindingSource::kPromptEveryTime:
            return "prompt-every-time";
        case KdbxBindingSource::kKeePassUuid:
            return "keepass-uuid";
        case KdbxBindingSource::kKeePassAutoMatch:
            return "keepass-auto-match";
    }
    return "session-saved";
}

bool kdbxBindingSourceFromString(const std::string& text, KdbxBindingSource& out) noexcept {
    if (text == "session-saved") {
        out = KdbxBindingSource::kSessionSaved;
        return true;
    }
    if (text == "prompt-every-time") {
        out = KdbxBindingSource::kPromptEveryTime;
        return true;
    }
    if (text == "keepass-uuid") {
        out = KdbxBindingSource::kKeePassUuid;
        return true;
    }
    if (text == "keepass-auto-match") {
        out = KdbxBindingSource::kKeePassAutoMatch;
        return true;
    }
    return false;
}

KdbxResolution resolveKdbxBinding(const KdbxIndex& index, const KdbxBinding& binding,
                                  const std::string& session_host) {
    KdbxResolution resolution;

    switch (binding.source) {
        case KdbxBindingSource::kSessionSaved:
            // The password belongs to the session, not to the database.
            resolution.outcome = KdbxResolutionOutcome::kNotFound;
            return resolution;

        case KdbxBindingSource::kPromptEveryTime:
            resolution.outcome = KdbxResolutionOutcome::kNeedsPrompt;
            return resolution;

        case KdbxBindingSource::kKeePassUuid: {
            for (std::size_t i = 0; i < index.entries.size(); ++i) {
                if (index.entries[i].uuid == binding.entry_uuid) {
                    resolution.outcome = KdbxResolutionOutcome::kFound;
                    resolution.entry_index = i;
                    return resolution;
                }
            }
            resolution.outcome = KdbxResolutionOutcome::kNotFound;
            return resolution;
        }

        case KdbxBindingSource::kKeePassAutoMatch: {
            const std::string& host =
                binding.match_host.empty() ? session_host : binding.match_host;
            for (std::size_t i = 0; i < index.entries.size(); ++i) {
                if (urlMatchesHost(index.entries[i].url, host)) {
                    resolution.outcome = KdbxResolutionOutcome::kFound;
                    resolution.entry_index = i;
                    return resolution;
                }
                if (!binding.match_host.empty() &&
                    urlMatchesHost(index.entries[i].url, binding.match_host)) {
                    resolution.outcome = KdbxResolutionOutcome::kFound;
                    resolution.entry_index = i;
                    return resolution;
                }
            }
            resolution.outcome = KdbxResolutionOutcome::kNotFound;
            return resolution;
        }
    }

    resolution.outcome = KdbxResolutionOutcome::kNotFound;
    return resolution;
}

}  // namespace openscrt::credential::kdbx
