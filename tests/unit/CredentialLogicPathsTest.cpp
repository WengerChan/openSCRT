// The logic of the credential library that the other tests do not walk.
//
// The reader, the vault and the client are covered by the tests that talk to a real
// database and to a real plugin. What is here is the logic underneath them: the base64
// the protocol is built on, the way a binding is resolved against an index, and the
// names the modules answer with.
//
// These are small functions, and small functions are where a wrong answer hides: a
// base64 that does not round-trip, a host that is matched with its port still attached,
// a name that does not survive the trip through a file.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/BuildInfo.h"
#include "core/ModuleInfo.h"
#include "credential/ModuleInfo.h"
#include "credential/kdbx/KdbxCredentialResolver.h"
#include "credential/keepasshttp/KeePassHttpCrypto.h"
#include "protocols/ModuleInfo.h"
#include "terminal/ModuleInfo.h"

namespace openscrt::credential::keepasshttp {
namespace {

// ------------------------------------------------------------------ the base64

TEST(KeePassHttpCryptoPathsTest, Base64RoundTrips) {
    const std::string text = "a value that is a bit longer than one block";
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());

    const std::string encoded = base64(bytes.data(), bytes.size());
    EXPECT_FALSE(encoded.empty());

    std::vector<std::uint8_t> decoded;
    ASSERT_TRUE(decodeBase64(encoded, decoded));
    EXPECT_EQ(std::string(decoded.begin(), decoded.end()), text);
}

TEST(KeePassHttpCryptoPathsTest, Base64KeepsThePadding) {
    // One, two and three bytes: the padding is what tells the length apart, and a
    // decoder that drops the wrong number of bytes turns "a" into two nulls.
    for (std::size_t length = 1; length <= 3; ++length) {
        const std::vector<std::uint8_t> bytes(length, 0x41);
        const std::string encoded = base64(bytes.data(), bytes.size());
        std::vector<std::uint8_t> decoded;
        ASSERT_TRUE(decodeBase64(encoded, decoded)) << encoded;
        EXPECT_EQ(decoded.size(), length) << "the padding was not honoured for " << encoded;
    }
}

TEST(KeePassHttpCryptoPathsTest, Base64RefusesWhatIsNotBase64) {
    std::vector<std::uint8_t> decoded;
    EXPECT_FALSE(decodeBase64("not base64 at all!", decoded));
    // An empty string is an empty value, not an error.
    EXPECT_TRUE(decodeBase64("", decoded));
    EXPECT_TRUE(decoded.empty());
}

TEST(KeePassHttpCryptoPathsTest, AFieldWithTheWrongKeyOrNonceIsRefused) {
    const std::vector<std::uint8_t> key(kKeyBytes, 0x11);
    const std::vector<std::uint8_t> nonce(kNonceBytes, 0x22);
    const std::string key_text = base64(key.data(), key.size());
    const std::string nonce_text = base64(nonce.data(), nonce.size());

    std::string out;
    std::string error;

    // A key that is not base64 at all.
    EXPECT_FALSE(encryptField("not base64!", nonce_text, "a value", out, error));
    EXPECT_NE(error.find("base64"), std::string::npos) << error;

    // A key of the right encoding but the wrong length.
    error.clear();
    const std::vector<std::uint8_t> short_key(16, 0x11);
    EXPECT_FALSE(encryptField(base64(short_key.data(), short_key.size()), nonce_text, "a value",
                              out, error));
    EXPECT_NE(error.find("length"), std::string::npos) << error;

    // A nonce of the wrong length.
    error.clear();
    const std::vector<std::uint8_t> short_nonce(8, 0x22);
    EXPECT_FALSE(encryptField(key_text, base64(short_nonce.data(), short_nonce.size()), "a value",
                              out, error));
    EXPECT_NE(error.find("length"), std::string::npos) << error;

    // The right key and nonce work.
    error.clear();
    ASSERT_TRUE(encryptField(key_text, nonce_text, "a value", out, error)) << error;
    EXPECT_FALSE(out.empty());
}

}  // namespace
}  // namespace openscrt::credential::keepasshttp

namespace openscrt::credential::kdbx {
namespace {

// ------------------------------------------------------------------ the binding

KdbxIndex indexWithOneEntry() {
    KdbxIndex index;
    KdbxEntry entry;
    entry.uuid = "ZW50cnk=";
    entry.title = "A login";
    entry.url = "ssh://10.20.30.40:22";
    index.entries.push_back(entry);
    return index;
}

TEST(KdbxCredentialResolverPathsTest, TheSourceNamesRoundTrip) {
    const std::vector<KdbxBindingSource> sources = {
        KdbxBindingSource::kSessionSaved,
        KdbxBindingSource::kPromptEveryTime,
        KdbxBindingSource::kKeePassUuid,
        KdbxBindingSource::kKeePassAutoMatch,
    };
    for (KdbxBindingSource source : sources) {
        KdbxBindingSource parsed{};
        ASSERT_TRUE(kdbxBindingSourceFromString(toString(source), parsed)) << toString(source);
        EXPECT_EQ(parsed, source);
    }

    KdbxBindingSource parsed{};
    EXPECT_FALSE(kdbxBindingSourceFromString("no-such-source", parsed));
}

TEST(KdbxCredentialResolverPathsTest, ABindingResolvesAgainstTheIndex) {
    const KdbxIndex index = indexWithOneEntry();

    KdbxBinding binding;
    binding.source = KdbxBindingSource::kKeePassUuid;
    binding.entry_uuid = "ZW50cnk=";
    KdbxResolution resolution = resolveKdbxBinding(index, binding, "10.20.30.40");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kFound);
    EXPECT_NE(resolution.entry(index), nullptr);

    // A binding that points at an entry that is not there.
    binding.entry_uuid = "bm90IGhlcmU=";
    resolution = resolveKdbxBinding(index, binding, "10.20.30.40");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kNotFound);
    EXPECT_EQ(resolution.entry(index), nullptr);

    // The session's own password: it is not in the database, so the index has nothing
    // to offer.
    binding.source = KdbxBindingSource::kSessionSaved;
    resolution = resolveKdbxBinding(index, binding, "10.20.30.40");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kNotFound);

    // The session asks every time.
    binding.source = KdbxBindingSource::kPromptEveryTime;
    resolution = resolveKdbxBinding(index, binding, "10.20.30.40");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kNeedsPrompt);
}

TEST(KdbxCredentialResolverPathsTest, TheAutoMatchAlsoMatchesANameThatEndsWithTheHost) {
    // "router.example" and "router.example.local" are the same machine as far as a
    // password is concerned.
    KdbxIndex index;
    KdbxEntry entry;
    entry.uuid = "ZW50cnk=";
    entry.url = "ssh://router.example.local:22";
    index.entries.push_back(entry);

    KdbxBinding binding;
    binding.source = KdbxBindingSource::kKeePassAutoMatch;

    KdbxResolution resolution = resolveKdbxBinding(index, binding, "router.example");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kFound);

    // The other way round: the session's host is the longer one.
    resolution = resolveKdbxBinding(index, binding, "router.example.local");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kFound);

    // A host that is not the same machine at all.
    resolution = resolveKdbxBinding(index, binding, "somewhere.else");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kNotFound);
}

TEST(KdbxCredentialResolverPathsTest, TheAutoMatchIgnoresThePathOfTheUrl) {
    KdbxIndex index;
    KdbxEntry entry;
    entry.uuid = "ZW50cnk=";
    entry.url = "ssh://10.20.30.40:22/some/path";
    index.entries.push_back(entry);

    KdbxBinding binding;
    binding.source = KdbxBindingSource::kKeePassAutoMatch;

    const KdbxResolution resolution = resolveKdbxBinding(index, binding, "10.20.30.40");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kFound);
}

TEST(KdbxCredentialResolverPathsTest, TheAutoMatchIgnoresTheSchemeAndThePort) {
    const KdbxIndex index = indexWithOneEntry();

    KdbxBinding binding;
    binding.source = KdbxBindingSource::kKeePassAutoMatch;

    // The host of the session, without a scheme or a port.
    KdbxResolution resolution = resolveKdbxBinding(index, binding, "10.20.30.40");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kFound);

    // A host of its own in the binding. It is a host, not a url: the scheme and the
    // port are stripped from the *entry's* url, not from this one.
    binding.match_host = "10.20.30.40";
    resolution = resolveKdbxBinding(index, binding, "something.else");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kFound);

    // A host that is not in the index at all.
    binding.match_host.clear();
    resolution = resolveKdbxBinding(index, binding, "10.99.99.99");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kNotFound);
}

}  // namespace
}  // namespace openscrt::credential::kdbx

namespace openscrt::core {
namespace {

// Every module says which one it is, and answers with the project's version rather than
// a second number that drifts from the installer.
TEST(ModuleInfoTest, EveryModuleSaysItsName) {
    EXPECT_STREQ(openscrt::core::moduleName(), "openscrt_core");
    EXPECT_STREQ(openscrt::terminal::moduleName(), "openscrt_terminal");
    EXPECT_STREQ(openscrt::credential::moduleName(), "openscrt_credential");
    EXPECT_STREQ(openscrt::protocols::moduleName(), "openscrt_protocols");
}

TEST(ModuleInfoTest, EveryModuleAnswersWithTheProjectVersion) {
    EXPECT_STREQ(openscrt::core::moduleVersion(), openscrt::core::kProjectVersion);
    EXPECT_STREQ(openscrt::terminal::moduleVersion(), openscrt::core::kProjectVersion);
    EXPECT_STREQ(openscrt::credential::moduleVersion(), openscrt::core::kProjectVersion);
    EXPECT_STREQ(openscrt::protocols::moduleVersion(), openscrt::core::kProjectVersion);
}

}  // namespace
}  // namespace openscrt::core
