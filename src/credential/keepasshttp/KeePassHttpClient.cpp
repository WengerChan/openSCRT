#include "credential/keepasshttp/KeePassHttpClient.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <sodium.h>

#include <cstring>

#include "credential/keepasshttp/KeePassHttpCrypto.h"

namespace openscrt::credential::keepasshttp {
namespace {

using nlohmann::json;

// A fresh nonce, base64'd, and the verifier that proves the key: the nonce encrypted
// with itself.
[[nodiscard]] bool makeVerifier(const std::string& key_base64, std::string& nonce_base64,
                                std::string& verifier_base64, std::string& error) {
    std::uint8_t nonce[kNonceBytes];
    randombytes_buf(nonce, sizeof(nonce));
    nonce_base64 = base64(nonce, sizeof(nonce));
    return encryptField(key_base64, nonce_base64, nonce_base64, verifier_base64, error);
}

}  // namespace

KeePassHttpClient::KeePassHttpClient(std::shared_ptr<IHttpTransport> transport)
    : transport_(std::move(transport)) {}

bool KeePassHttpClient::associate(Association& association, std::string& error) {
    if (sodium_init() < 0) {
        error = "libsodium could not be initialised";
        return false;
    }
    std::uint8_t key[kKeyBytes];
    randombytes_buf(key, sizeof(key));
    const std::string key_base64 = base64(key, sizeof(key));
    sodium_memzero(key, sizeof(key));

    std::string nonce_base64;
    std::string verifier_base64;
    if (!makeVerifier(key_base64, nonce_base64, verifier_base64, error)) {
        return false;
    }

    json request = {{"RequestType", "associate"},
                    {"Key", key_base64},
                    {"Nonce", nonce_base64},
                    {"Verifier", verifier_base64}};

    std::string response_body;
    if (!transport_->post(request.dump(), response_body, error)) {
        return false;
    }

    json response;
    try {
        response = json::parse(response_body);
    } catch (const std::exception&) {
        error = "the plugin did not answer with JSON";
        return false;
    }
    if (!response.contains("Id") || !response["Id"].is_string()) {
        error = "the plugin did not assign an id (the association was refused?)";
        return false;
    }
    association.key_id = response["Id"].get<std::string>();
    association.key_base64 = key_base64;
    return true;
}

bool KeePassHttpClient::testAssociate(const Association& association, std::string& error) {
    std::string nonce_base64;
    std::string verifier_base64;
    if (!makeVerifier(association.key_base64, nonce_base64, verifier_base64, error)) {
        return false;
    }
    json request = {{"RequestType", "test-associate"},
                    {"Id", association.key_id},
                    {"Nonce", nonce_base64},
                    {"Verifier", verifier_base64}};

    std::string response_body;
    if (!transport_->post(request.dump(), response_body, error)) {
        return false;
    }
    json response;
    try {
        response = json::parse(response_body);
    } catch (const std::exception&) {
        error = "the plugin did not answer with JSON";
        return false;
    }
    if (!response.contains("Result") || !response["Result"].is_boolean() ||
        !response["Result"].get<bool>()) {
        error = "the plugin does not know this association any more";
        return false;
    }
    return true;
}

bool KeePassHttpClient::getLogins(const Association& association, const std::string& url,
                                  const std::string& host, std::vector<KeePassHttpEntry>& entries,
                                  std::string& error) {
    std::string nonce_base64;
    std::string verifier_base64;
    if (!makeVerifier(association.key_base64, nonce_base64, verifier_base64, error)) {
        return false;
    }
    std::string url_base64;
    std::string host_base64;
    if (!encryptField(association.key_base64, nonce_base64, url, url_base64, error)) {
        return false;
    }
    if (!encryptField(association.key_base64, nonce_base64, host, host_base64, error)) {
        return false;
    }

    json request = {{"RequestType", "get-logins"}, {"Id", association.key_id},
                    {"Nonce", nonce_base64},       {"Verifier", verifier_base64},
                    {"Url", url_base64},           {"SubmitUrl", host_base64}};

    std::string response_body;
    if (!transport_->post(request.dump(), response_body, error)) {
        return false;
    }
    json response;
    try {
        response = json::parse(response_body);
    } catch (const std::exception&) {
        error = "the plugin did not answer with JSON";
        return false;
    }
    if (!response.contains("Nonce") || !response["Nonce"].is_string()) {
        error = "the plugin did not answer with a nonce";
        return false;
    }
    // The answer is encrypted with the *response's* nonce, not with ours.
    const std::string response_nonce = response["Nonce"].get<std::string>();

    entries.clear();
    if (!response.contains("Entries") || !response["Entries"].is_array()) {
        // No entries is a valid answer: the plugin simply has nothing for this URL.
        return true;
    }
    for (const json& item : response["Entries"]) {
        KeePassHttpEntry entry;
        std::string plain;
        const auto readField = [&](const char* name, std::string& target) {
            if (!item.contains(name) || !item[name].is_string()) {
                return true;  // the field is simply not there
            }
            if (!decryptField(association.key_base64, response_nonce, item[name].get<std::string>(),
                              plain, error)) {
                return false;
            }
            target = plain;
            return true;
        };
        if (!readField("Name", entry.name) || !readField("Login", entry.login) ||
            !readField("Password", entry.password)) {
            return false;
        }
        if (item.contains("StringFields") && item["StringFields"].is_object()) {
            for (const auto& [name, value] : item["StringFields"].items()) {
                std::string plain_field;
                if (!value.is_string()) {
                    continue;
                }
                if (!decryptField(association.key_base64, response_nonce, value.get<std::string>(),
                                  plain_field, error)) {
                    return false;
                }
                entry.string_fields.emplace_back(name, plain_field);
            }
        }
        entries.push_back(std::move(entry));
    }
    return true;
}

}  // namespace openscrt::credential::keepasshttp
