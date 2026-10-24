#include "credential/ICredentialProvider.h"

namespace openscrt::credential {

const char* toString(CredentialField field) noexcept {
    switch (field) {
        case CredentialField::kPassword:
            return "password";
        case CredentialField::kPrivateKeyPassphrase:
            return "private-key-passphrase";
        case CredentialField::kJumpHostPassword:
            return "jump-host-password";
        case CredentialField::kSudoPassword:
            return "sudo-password";
        case CredentialField::kTotp:
            return "totp";
    }
    return "password";
}

const char* toString(CredentialSource source) noexcept {
    switch (source) {
        case CredentialSource::kSessionSaved:
            return "session-saved";
        case CredentialSource::kPromptEveryTime:
            return "prompt-every-time";
        case CredentialSource::kKeePassUuid:
            return "keepass-uuid";
        case CredentialSource::kKeePassAutoMatch:
            return "keepass-auto-match";
    }
    return "session-saved";
}

std::optional<CredentialSource> credentialSourceFromString(std::string_view text) noexcept {
    if (text == "session-saved") {
        return CredentialSource::kSessionSaved;
    }
    if (text == "prompt-every-time") {
        return CredentialSource::kPromptEveryTime;
    }
    if (text == "keepass-uuid") {
        return CredentialSource::kKeePassUuid;
    }
    if (text == "keepass-auto-match") {
        return CredentialSource::kKeePassAutoMatch;
    }
    return std::nullopt;
}

const char* toString(CredentialStatus status) noexcept {
    switch (status) {
        case CredentialStatus::kFound:
            return "found";
        case CredentialStatus::kNotFound:
            return "not-found";
        case CredentialStatus::kCancelled:
            return "cancelled";
        case CredentialStatus::kUnavailable:
            return "unavailable";
        case CredentialStatus::kError:
            return "error";
    }
    return "error";
}

bool takeSecret(CredentialResult& result, core::SecureByteArray& out) {
    if (result.status != CredentialStatus::kFound) {
        return false;
    }
    if (result.entry.password.empty()) {
        return false;
    }
    out = std::move(result.entry.password);
    return true;
}

}  // namespace openscrt::credential
