#include "credential/SecretStore.h"

#include <algorithm>

#if defined(_WIN32)
#include <windows.h>
// The comment between the two keeps clang-format from putting them in the wrong order:
// wincred.h uses the types of windows.h and does not include it itself.
#include <wincred.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace openscrt::credential {
namespace {

// The name of the service the secrets of this program are stored under. The keychain of
// macOS keeps it beside the name of the entry, so that the entries of this program are
// together.
constexpr const char* kServiceName = "openSCRT";

#if defined(_WIN32)

// The Credential Manager keeps the name as UTF-16, and the names this program uses are
// ASCII.
std::wstring widen(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

class WindowsCredentialStore final : public ISecretStore {
 public:
    [[nodiscard]] bool store(const std::string& name, const std::vector<std::uint8_t>& secret,
                             std::string& error) override {
        const std::wstring target = widen(name);
        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = target.c_str();
        credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
        // The API writes the blob but does not change it. An empty secret still needs a
        // pointer the API can look at.
        const BYTE* blob = secret.empty() ? reinterpret_cast<const BYTE*>("")
                                          : reinterpret_cast<const BYTE*>(secret.data());
        credential.CredentialBlob = const_cast<BYTE*>(blob);
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        credential.Comment = L"openSCRT";
        credential.UserName = L"openSCRT";
        if (!CredWriteW(&credential, 0)) {
            error = "the credential manager refused the secret";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool load(const std::string& name, std::vector<std::uint8_t>& secret,
                            std::string& error) override {
        const std::wstring target = widen(name);
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
            error = "there is no secret stored under that name";
            return false;
        }
        const BYTE* blob = credential->CredentialBlob;
        const DWORD size = credential->CredentialBlobSize;
        secret.assign(blob, blob + size);
        CredFree(credential);
        return true;
    }

    [[nodiscard]] bool remove(const std::string& name, std::string& error) override {
        const std::wstring target = widen(name);
        if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
            error = "there was no secret stored under that name";
            return false;
        }
        return true;
    }
};

#elif defined(__APPLE__)

// The keychain keeps the name of the service as a CFString and the secret as CFData.
CFStringRef copyString(const std::string& text) {
    return CFStringCreateWithCString(kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
}

class MacKeychainStore final : public ISecretStore {
 public:
    [[nodiscard]] bool store(const std::string& name, const std::vector<std::uint8_t>& secret,
                             std::string& error) override {
        // The keychain refuses to add an item that is already there, and the caller is
        // allowed to store the same name again. There not being an old item is the
        // normal case, so the answer of the removal is not interesting -- but it has to
        // be cast away, because the interface says a caller has to look at it.
        std::string ignored;
        static_cast<void>(remove(name, ignored));

        CFStringRef service = copyString(kServiceName);
        CFStringRef account = copyString(name);
        const UInt8* bytes = secret.empty() ? reinterpret_cast<const UInt8*>("")
                                            : reinterpret_cast<const UInt8*>(secret.data());
        CFDataRef data =
            CFDataCreate(kCFAllocatorDefault, bytes, static_cast<CFIndex>(secret.size()));
        const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData};
        const void* values[] = {kSecClassGenericPassword, service, account, data};
        CFDictionaryRef query =
            CFDictionaryCreate(kCFAllocatorDefault, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
        const OSStatus status = SecItemAdd(query, nullptr);
        CFRelease(query);
        CFRelease(data);
        CFRelease(account);
        CFRelease(service);
        if (status != errSecSuccess) {
            error = "the keychain refused the secret";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool load(const std::string& name, std::vector<std::uint8_t>& secret,
                            std::string& error) override {
        CFStringRef service = copyString(kServiceName);
        CFStringRef account = copyString(name);
        const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData,
                              kSecMatchLimit};
        const void* values[] = {kSecClassGenericPassword, service, account, kCFBooleanTrue,
                                kSecMatchLimitOne};
        CFDictionaryRef query =
            CFDictionaryCreate(kCFAllocatorDefault, keys, values, 5, &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
        CFTypeRef found = nullptr;
        const OSStatus status = SecItemCopyMatching(query, &found);
        CFRelease(query);
        CFRelease(account);
        CFRelease(service);
        if (status == errSecItemNotFound) {
            error = "there is no secret stored under that name";
            return false;
        }
        if (status != errSecSuccess || found == nullptr) {
            error = "the keychain could not be read";
            return false;
        }
        CFDataRef data = static_cast<CFDataRef>(found);
        const CFIndex length = CFDataGetLength(data);
        const auto* bytes = static_cast<const std::uint8_t*>(CFDataGetBytePtr(data));
        secret.assign(bytes, bytes + length);
        CFRelease(found);
        return true;
    }

    [[nodiscard]] bool remove(const std::string& name, std::string& error) override {
        CFStringRef service = copyString(kServiceName);
        CFStringRef account = copyString(name);
        const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
        const void* values[] = {kSecClassGenericPassword, service, account};
        CFDictionaryRef query =
            CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
        const OSStatus status = SecItemDelete(query);
        CFRelease(query);
        CFRelease(account);
        CFRelease(service);
        if (status != errSecSuccess) {
            error = "there was no secret stored under that name";
            return false;
        }
        return true;
    }
};

#endif

}  // namespace

std::unique_ptr<ISecretStore> systemSecretStore() {
#if defined(_WIN32)
    return std::make_unique<WindowsCredentialStore>();
#elif defined(__APPLE__)
    return std::make_unique<MacKeychainStore>();
#else
    // Linux has Libsecret, which is not wired up yet. The caller keeps the secret in
    // memory for as long as the program runs.
    return nullptr;
#endif
}

bool MemorySecretStore::store(const std::string& name, const std::vector<std::uint8_t>& secret,
                              std::string& /*error*/) {
    for (auto& entry : secrets_) {
        if (entry.first == name) {
            entry.second = secret;
            return true;
        }
    }
    secrets_.emplace_back(name, secret);
    return true;
}

bool MemorySecretStore::load(const std::string& name, std::vector<std::uint8_t>& secret,
                             std::string& error) {
    for (const auto& entry : secrets_) {
        if (entry.first == name) {
            secret = entry.second;
            return true;
        }
    }
    error = "there is no secret stored under that name";
    return false;
}

bool MemorySecretStore::remove(const std::string& name, std::string& error) {
    const auto found = std::find_if(secrets_.begin(), secrets_.end(),
                                    [&name](const auto& entry) { return entry.first == name; });
    if (found == secrets_.end()) {
        error = "there was no secret stored under that name";
        return false;
    }
    secrets_.erase(found);
    return true;
}

}  // namespace openscrt::credential
