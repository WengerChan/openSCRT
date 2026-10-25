// The secret store of the system (P2-T8).
//
// Some things this program has to keep are secrets: the key of the AES session with the
// KeePassHttp plugin is one of them. A secret that is written into a file of ours is a
// secret that ends up in a backup, in a sync folder and in a support ticket, so the
// system has its own places for them -- the Credential Manager on Windows, the Keychain
// on macOS -- and this is the seam in front of them.
//
// The store is small on purpose: one name, one secret, no query language. What it is not
// is a place to keep a lot of things, because a place to keep a lot of things becomes a
// place where something ends up that should not be there.

#ifndef OPENCRT_CREDENTIAL_SECRET_STORE_H_
#define OPENCRT_CREDENTIAL_SECRET_STORE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace openscrt::credential {

class ISecretStore {
 public:
    virtual ~ISecretStore() = default;

    // Stores the secret under `name`, replacing what was there.
    [[nodiscard]] virtual bool store(const std::string& name,
                                     const std::vector<std::uint8_t>& secret,
                                     std::string& error) = 0;

    // Reads the secret back. Returns false when there is nothing under that name; the
    // error says which of the two it was.
    [[nodiscard]] virtual bool load(const std::string& name, std::vector<std::uint8_t>& secret,
                                    std::string& error) = 0;

    // Removes the secret. Returns false when there was nothing to remove.
    [[nodiscard]] virtual bool remove(const std::string& name, std::string& error) = 0;
};

// The secret store of the system this program runs on.
//
// Returns null when the platform has none that is wired up. That is not a failure of
// the program: the caller keeps the secret in memory for as long as the program runs and
// says so, instead of writing it somewhere it should not be.
[[nodiscard]] std::unique_ptr<ISecretStore> systemSecretStore();

// A store that keeps the secrets in memory, for the platforms that have none and for
// the tests.
class MemorySecretStore final : public ISecretStore {
 public:
    [[nodiscard]] bool store(const std::string& name, const std::vector<std::uint8_t>& secret,
                             std::string& error) override;
    [[nodiscard]] bool load(const std::string& name, std::vector<std::uint8_t>& secret,
                            std::string& error) override;
    [[nodiscard]] bool remove(const std::string& name, std::string& error) override;

 private:
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> secrets_;
};

}  // namespace openscrt::credential

#endif  // OPENCRT_CREDENTIAL_SECRET_STORE_H_
