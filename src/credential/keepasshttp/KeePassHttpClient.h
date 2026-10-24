// The KeePassHTTP client (P2-T8).
//
// KeePassHTTP is how a terminal asks a running KeePass for a password: the plugin
// listens on 127.0.0.1:19455, the client associates with it once, and after that every
// request is a small JSON object whose sensitive fields are encrypted with the shared
// key.
//
// The protocol has three requests:
//
//   associate       the client offers a key and a nonce; the user confirms the prompt
//                   in KeePass; the plugin answers with the id it assigned.
//   test-associate  checks that the association still works.
//   get-logins      asks for the entries of a URL; the answers come back encrypted
//                   with the *response's* nonce.
//
// The encryption is AES-256-CBC with PKCS#7 padding, and the base64 of the encrypted
// nonce is the "Verifier" -- the proof that the other side holds the key.

#ifndef OPENCRT_CREDENTIAL_KEEPASSHTTP_KEEPASSHTTP_CLIENT_H_
#define OPENCRT_CREDENTIAL_KEEPASSHTTP_KEEPASSHTTP_CLIENT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace openscrt::credential::keepasshttp {

// The transport the client talks through.
//
// It is an interface so the protocol can be tested without a KeePass running, and so
// the socket code stays in one place.
class IHttpTransport {
 public:
    virtual ~IHttpTransport() = default;

    // POSTs `body` and puts the response body into `response`.
    [[nodiscard]] virtual bool post(const std::string& body, std::string& response,
                                    std::string& error) = 0;
};

// The association between this client and the plugin: the id the plugin assigned and
// the shared key. The key is base64, the way the protocol carries it.
struct Association {
    std::string key_id;
    std::string key_base64;
};

// One entry as the plugin returns it. The fields arrive encrypted and are decrypted
// here.
struct KeePassHttpEntry {
    std::string name;
    std::string login;
    std::string password;
    std::vector<std::pair<std::string, std::string>> string_fields;
};

class KeePassHttpClient {
 public:
    explicit KeePassHttpClient(std::shared_ptr<IHttpTransport> transport);

    // Associates with the plugin. The user has to confirm the prompt in KeePass, so
    // this is the call that can block on a human.
    [[nodiscard]] bool associate(Association& association, std::string& error);

    // Checks that the association still works. A plugin that was restarted or a
    // database that was locked answers with a failure here.
    [[nodiscard]] bool testAssociate(const Association& association, std::string& error);

    // Looks up the entries for a URL. `host` is the SubmitUrl, the host part of the
    // session; `url` is the full URL, which is what the entries are matched against.
    [[nodiscard]] bool getLogins(const Association& association, const std::string& url,
                                 const std::string& host, std::vector<KeePassHttpEntry>& entries,
                                 std::string& error);

 private:
    std::shared_ptr<IHttpTransport> transport_;
};

}  // namespace openscrt::credential::keepasshttp

#endif  // OPENCRT_CREDENTIAL_KEEPASSHTTP_KEEPASSHTTP_CLIENT_H_
