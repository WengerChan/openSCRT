#include "credential/keepasshttp/KeePassHttpProvider.h"

#include <utility>
#include <vector>

namespace openscrt::credential::keepasshttp {

KeePassHttpProvider::KeePassHttpProvider(std::shared_ptr<IHttpTransport> transport,
                                         Association association)
    : client_(std::move(transport)), association_(std::move(association)) {}

bool KeePassHttpProvider::isAvailable() const noexcept {
    return !association_.key_id.empty() && !association_.key_base64.empty();
}

CredentialResult KeePassHttpProvider::lookup(const CredentialRequest& request) {
    CredentialResult result;
    if (!isAvailable()) {
        result.status = CredentialStatus::kUnavailable;
        result.message = "this program is not paired with a running KeePass yet";
        return result;
    }

    // The plugin matches the entries against the URL, and the host is what it answers
    // with. The URL is the one the session would use, so that an entry saved for
    // "ssh://host:22" is found by a session that goes to that host.
    const std::string url = "ssh://" + request.host + ":" + std::to_string(request.port);
    std::vector<KeePassHttpEntry> entries;
    std::string error;
    if (!client_.getLogins(association_, url, request.host, entries, error)) {
        result.status = CredentialStatus::kUnavailable;
        result.message = error;
        return result;
    }
    if (entries.empty()) {
        result.status = CredentialStatus::kNotFound;
        result.message = "KeePass has no entry for that host";
        return result;
    }

    const KeePassHttpEntry& entry = entries.front();
    result.status = CredentialStatus::kFound;
    result.entry.name = entry.name;
    result.entry.login = entry.login;
    result.entry.password.assign(entry.password.data(), entry.password.size());
    return result;
}

}  // namespace openscrt::credential::keepasshttp
