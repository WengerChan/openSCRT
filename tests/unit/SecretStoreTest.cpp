// The secret store of the system (P2-T8).
//
// The association with the KeePassHttp plugin holds the key of the AES session, so it
// is a secret and it belongs in the store of the system rather than in a file of ours.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "credential/SecretStore.h"
#include "credential/keepasshttp/KeePassHttpAssociation.h"

namespace openscrt::credential {
namespace {

std::vector<std::uint8_t> bytesOf(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

TEST(SecretStoreTest, TheMemoryStoreKeepsASecret) {
    MemorySecretStore store;
    std::string error;

    const std::vector<std::uint8_t> secret = bytesOf("a shared key");
    ASSERT_TRUE(store.store("one", secret, error)) << error;

    std::vector<std::uint8_t> loaded;
    ASSERT_TRUE(store.load("one", loaded, error)) << error;
    EXPECT_EQ(loaded, secret);

    // Storing the same name again replaces what was there.
    const std::vector<std::uint8_t> other = bytesOf("another key");
    ASSERT_TRUE(store.store("one", other, error)) << error;
    ASSERT_TRUE(store.load("one", loaded, error)) << error;
    EXPECT_EQ(loaded, other);

    ASSERT_TRUE(store.remove("one", error)) << error;
    EXPECT_FALSE(store.load("one", loaded, error));
    EXPECT_FALSE(error.empty()) << "a refusal has to say why";
    EXPECT_FALSE(store.remove("one", error));
}

TEST(SecretStoreTest, TheAssociationSurvivesTheStore) {
    MemorySecretStore store;
    std::string error;

    keepasshttp::Association association;
    association.key_id = "the id the plugin assigned";
    association.key_base64 = "the shared key, base64";

    ASSERT_TRUE(keepasshttp::saveAssociation(store, association, error)) << error;

    keepasshttp::Association loaded;
    ASSERT_TRUE(keepasshttp::loadAssociation(store, loaded, error)) << error;
    EXPECT_EQ(loaded.key_id, association.key_id);
    EXPECT_EQ(loaded.key_base64, association.key_base64);

    ASSERT_TRUE(keepasshttp::forgetAssociation(store, error)) << error;
    keepasshttp::Association gone;
    EXPECT_FALSE(keepasshttp::loadAssociation(store, gone, error));
}

TEST(SecretStoreTest, TheSystemStoreKeepsASecret) {
    std::unique_ptr<ISecretStore> store = systemSecretStore();
    if (store == nullptr) {
        GTEST_SKIP() << "this platform has no secret store wired up yet";
    }

    // A name of its own, so the test never touches what the program itself stored.
    const std::string name = "openSCRT/test/secret-store";
    const std::vector<std::uint8_t> secret = bytesOf("a shared key");
    std::string error;

    if (!store->store(name, secret, error)) {
        // A keychain in a runner without a desktop may refuse to be talked to. That is
        // the platform saying no, not the program being wrong.
        GTEST_SKIP() << "the secret store of this machine refused the secret: " << error;
    }

    std::vector<std::uint8_t> loaded;
    ASSERT_TRUE(store->load(name, loaded, error)) << error;
    EXPECT_EQ(loaded, secret);

    ASSERT_TRUE(store->remove(name, error)) << error;
    EXPECT_FALSE(store->load(name, loaded, error));
}

}  // namespace
}  // namespace openscrt::credential
