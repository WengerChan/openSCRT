// Credential provider abstraction tests (P2-T1).
//
// The interface itself is mostly vocabulary and one contract: the plaintext leaves the
// provider in a locked buffer and the provider does not keep a copy. The contract is worth
// a test because it is the one that is easy to break later: the day somebody adds a
// `std::string password` to CredentialEntry "just for logging", the memory guarantee
// (P2-A3) quietly goes away and no compiler complains.
//
// No Qt, no display: the credential layer is deliberately free of both, so it can be
// tested on its own.

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "credential/ICredentialProvider.h"

namespace openscrt::credential {
namespace {

// A provider that answers from a table, standing in for the .kdbx reader and the
// KeePassHTTP client. Implementing it here is part of the test: if the interface cannot
// be implemented without dragging in a database or an HTTP stack, the abstraction is
// in the wrong place.
class FakeProvider final : public ICredentialProvider {
 public:
    CredentialResult lookup(const CredentialRequest& request) override {
        last_request = request;
        ++calls;
        if (!available) {
            CredentialResult result;
            result.status = CredentialStatus::kUnavailable;
            result.message = "the provider cannot be reached";
            return result;
        }
        for (const auto& entry : entries) {
            if (entry.first == request.entry_uuid || entry.first == request.host) {
                CredentialResult result;
                result.status = CredentialStatus::kFound;
                result.entry.uuid = entry.first;
                result.entry.name = entry.second;
                result.entry.password.assign("s3cret");
                return result;
            }
        }
        CredentialResult result;
        result.status = CredentialStatus::kNotFound;
        result.message = "no entry matches";
        return result;
    }

    [[nodiscard]] std::string_view id() const noexcept override { return "fake"; }
    [[nodiscard]] bool isAvailable() const noexcept override { return available; }

    void lock() noexcept override {
        ++locks;
        entries.clear();
        // Locking drops the key material, so the provider cannot answer until it is
        // unlocked again -- the file or the service is still there, but the answer is
        // not. isAvailable() means "can answer right now", so it has to go false.
        available = false;
    }

    bool available{true};
    int calls{0};
    int locks{0};
    CredentialRequest last_request;
    std::vector<std::pair<std::string, std::string>> entries;
};

TEST(CredentialVocabularyTest, EveryFieldHasAName) {
    EXPECT_STREQ(toString(CredentialField::kPassword), "password");
    EXPECT_STREQ(toString(CredentialField::kPrivateKeyPassphrase), "private-key-passphrase");
    EXPECT_STREQ(toString(CredentialField::kJumpHostPassword), "jump-host-password");
    EXPECT_STREQ(toString(CredentialField::kSudoPassword), "sudo-password");
    EXPECT_STREQ(toString(CredentialField::kTotp), "totp");
}

TEST(CredentialVocabularyTest, EverySourceRoundTripsThroughItsName) {
    const CredentialSource sources[] = {
        CredentialSource::kSessionSaved,
        CredentialSource::kPromptEveryTime,
        CredentialSource::kKeePassUuid,
        CredentialSource::kKeePassAutoMatch,
    };
    for (const CredentialSource source : sources) {
        const std::optional<CredentialSource> parsed = credentialSourceFromString(toString(source));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, source);
    }
}

TEST(CredentialVocabularyTest, AnUnknownSourceNameIsNotAValidSource) {
    // A profile written by a newer version must not silently become "session saved",
    // because that changes where a password comes from without telling anybody.
    EXPECT_FALSE(credentialSourceFromString("keepass-auto-match-v2").has_value());
    EXPECT_FALSE(credentialSourceFromString("").has_value());
    EXPECT_FALSE(credentialSourceFromString("Session-Saved").has_value());
}

TEST(CredentialVocabularyTest, EveryStatusHasAName) {
    EXPECT_STREQ(toString(CredentialStatus::kFound), "found");
    EXPECT_STREQ(toString(CredentialStatus::kNotFound), "not-found");
    EXPECT_STREQ(toString(CredentialStatus::kCancelled), "cancelled");
    EXPECT_STREQ(toString(CredentialStatus::kUnavailable), "unavailable");
    EXPECT_STREQ(toString(CredentialStatus::kError), "error");
}

TEST(TakeSecretTest, ThePlaintextMovesToTheCallerAndLeavesTheResult) {
    CredentialResult result;
    result.status = CredentialStatus::kFound;
    result.entry.password.assign("s3cret");

    core::SecureByteArray received;
    ASSERT_TRUE(takeSecret(result, received));
    EXPECT_EQ(received.size(), 6u);
    EXPECT_TRUE(result.entry.password.empty());

    char buffer[7] = {};
    EXPECT_EQ(received.copyOut(buffer, sizeof(buffer)), 6u);
    EXPECT_STREQ(buffer, "s3cret");
}

TEST(TakeSecretTest, AFailedLookupHandsOverNothing) {
    CredentialResult result;
    result.status = CredentialStatus::kNotFound;

    core::SecureByteArray received;
    received.assign("previous");
    EXPECT_FALSE(takeSecret(result, received));
    // The caller's buffer is untouched: a failed lookup must not clear a password the
    // caller already had.
    EXPECT_EQ(received.size(), 8u);
}

TEST(TakeSecretTest, AnEntryWithoutAPasswordHandsOverNothing) {
    CredentialResult result;
    result.status = CredentialStatus::kFound;
    result.entry.name = "an entry with no password";

    core::SecureByteArray received;
    EXPECT_FALSE(takeSecret(result, received));
    EXPECT_TRUE(received.empty());
}

TEST(FakeProviderTest, TheRequestCarriesTheLookupKeyAndNoSecret) {
    FakeProvider provider;
    provider.entries.emplace_back("10.20.30.40", "router");

    CredentialRequest request;
    request.host = "10.20.30.40";
    request.port = 2222;
    request.user = "netops";
    request.entry_uuid = "entry-uuid";
    request.field = CredentialField::kPassword;

    const CredentialResult result = provider.lookup(request);
    EXPECT_EQ(result.status, CredentialStatus::kFound);
    EXPECT_EQ(provider.last_request.host, "10.20.30.40");
    EXPECT_EQ(provider.last_request.port, 2222);
    EXPECT_EQ(provider.last_request.user, "netops");
    EXPECT_EQ(provider.last_request.entry_uuid, "entry-uuid");
}

TEST(FakeProviderTest, AnUnreachableProviderSaysSoInsteadOfReturningAnEmptyPassword) {
    FakeProvider provider;
    provider.available = false;

    const CredentialResult result = provider.lookup(CredentialRequest{});
    EXPECT_EQ(result.status, CredentialStatus::kUnavailable);
    EXPECT_TRUE(result.entry.password.empty());
}

TEST(FakeProviderTest, LockIsReachableThroughTheInterface) {
    // P2-T7's auto-lock timer calls lock() on an ICredentialProvider pointer, so the
    // hook has to be part of the interface and not a cast to a concrete type.
    FakeProvider concrete;
    concrete.entries.emplace_back("10.20.30.40", "router");

    ICredentialProvider& provider = concrete;
    provider.lock();
    EXPECT_EQ(concrete.locks, 1);
    EXPECT_TRUE(concrete.entries.empty());
    EXPECT_FALSE(provider.isAvailable());
}

}  // namespace
}  // namespace openscrt::credential
