// KDBX credential binding tests (P2-T9).
//
// The binding is what a session says about where its password comes from. Resolving it
// is pure logic, so it is tested without a display: the only thing that needs a window
// is the prompt, and that is reported as "needs a prompt" rather than asked here.

#include <gtest/gtest.h>

#include <string>

#include "credential/kdbx/KdbxCredentialResolver.h"

namespace openscrt::credential::kdbx {
namespace {

KdbxIndex makeIndex() {
    KdbxIndex index;
    index.root_group_uuid = "root-uuid";

    KdbxEntry router;
    router.uuid = "uuid-router";
    router.group_path = "Root";
    router.title = "router";
    router.username = "netops";
    router.url = "ssh://10.20.30.40:22";
    index.entries.push_back(router);

    KdbxEntry byName;
    byName.uuid = "uuid-by-name";
    byName.group_path = "Root/Homebanking";
    byName.title = "bank";
    byName.username = "cash";
    byName.url = "bank.example";
    index.entries.push_back(byName);

    KdbxEntry noUrl;
    noUrl.uuid = "uuid-no-url";
    noUrl.group_path = "Root";
    noUrl.title = "no url";
    noUrl.username = "nobody";
    index.entries.push_back(noUrl);

    return index;
}

TEST(KdbxCredentialResolverTest, EverySourceHasAName) {
    KdbxBindingSource source = KdbxBindingSource::kSessionSaved;
    EXPECT_TRUE(kdbxBindingSourceFromString("session-saved", source));
    EXPECT_EQ(source, KdbxBindingSource::kSessionSaved);
    EXPECT_TRUE(kdbxBindingSourceFromString("prompt-every-time", source));
    EXPECT_EQ(source, KdbxBindingSource::kPromptEveryTime);
    EXPECT_TRUE(kdbxBindingSourceFromString("keepass-uuid", source));
    EXPECT_EQ(source, KdbxBindingSource::kKeePassUuid);
    EXPECT_TRUE(kdbxBindingSourceFromString("keepass-auto-match", source));
    EXPECT_EQ(source, KdbxBindingSource::kKeePassAutoMatch);

    // A name this build does not know must not become a binding of its own.
    EXPECT_FALSE(kdbxBindingSourceFromString("keepass-auto-match-v2", source));
}

TEST(KdbxCredentialResolverTest, ASessionSavedPasswordIsNotInTheDatabase) {
    const KdbxIndex index = makeIndex();
    KdbxBinding binding;
    binding.source = KdbxBindingSource::kSessionSaved;

    const KdbxResolution resolution = resolveKdbxBinding(index, binding, "10.20.30.40");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kNotFound);
    EXPECT_EQ(resolution.entry(index), nullptr);
}

TEST(KdbxCredentialResolverTest, PromptEveryTimeAsksTheUser) {
    const KdbxIndex index = makeIndex();
    KdbxBinding binding;
    binding.source = KdbxBindingSource::kPromptEveryTime;

    const KdbxResolution resolution = resolveKdbxBinding(index, binding, "10.20.30.40");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kNeedsPrompt);
}

TEST(KdbxCredentialResolverTest, AUuidBindingFindsItsEntry) {
    const KdbxIndex index = makeIndex();
    KdbxBinding binding;
    binding.source = KdbxBindingSource::kKeePassUuid;
    binding.entry_uuid = "uuid-by-name";

    const KdbxResolution resolution = resolveKdbxBinding(index, binding, "10.20.30.40");
    ASSERT_EQ(resolution.outcome, KdbxResolutionOutcome::kFound);
    ASSERT_NE(resolution.entry(index), nullptr);
    EXPECT_EQ(resolution.entry(index)->title, "bank");
}

TEST(KdbxCredentialResolverTest, AUuidThatIsNotThereIsReported) {
    const KdbxIndex index = makeIndex();
    KdbxBinding binding;
    binding.source = KdbxBindingSource::kKeePassUuid;
    binding.entry_uuid = "uuid-that-was-deleted";

    const KdbxResolution resolution = resolveKdbxBinding(index, binding, "10.20.30.40");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kNotFound);
}

TEST(KdbxCredentialResolverTest, AutoMatchUsesTheHostOfTheSession) {
    const KdbxIndex index = makeIndex();
    KdbxBinding binding;
    binding.source = KdbxBindingSource::kKeePassAutoMatch;

    const KdbxResolution resolution = resolveKdbxBinding(index, binding, "10.20.30.40");
    ASSERT_EQ(resolution.outcome, KdbxResolutionOutcome::kFound);
    EXPECT_EQ(resolution.entry(index)->title, "router");
}

TEST(KdbxCredentialResolverTest, AutoMatchIgnoresTheSchemeAndThePort) {
    const KdbxIndex index = makeIndex();
    KdbxBinding binding;
    binding.source = KdbxBindingSource::kKeePassAutoMatch;
    binding.match_host = "bank.example";

    const KdbxResolution resolution = resolveKdbxBinding(index, binding, "unrelated");
    ASSERT_EQ(resolution.outcome, KdbxResolutionOutcome::kFound);
    EXPECT_EQ(resolution.entry(index)->title, "bank");
}

TEST(KdbxCredentialResolverTest, AutoMatchPrefersTheBoundHost) {
    const KdbxIndex index = makeIndex();
    KdbxBinding binding;
    binding.source = KdbxBindingSource::kKeePassAutoMatch;
    binding.match_host = "bank.example";

    // The session's own host would match the router entry, but the binding names
    // another host, and the binding wins.
    const KdbxResolution resolution = resolveKdbxBinding(index, binding, "10.20.30.40");
    ASSERT_EQ(resolution.outcome, KdbxResolutionOutcome::kFound);
    EXPECT_EQ(resolution.entry(index)->title, "bank");
}

TEST(KdbxCredentialResolverTest, AutoMatchThatFindsNothingIsReported) {
    const KdbxIndex index = makeIndex();
    KdbxBinding binding;
    binding.source = KdbxBindingSource::kKeePassAutoMatch;
    binding.match_host = "nowhere.example";

    const KdbxResolution resolution = resolveKdbxBinding(index, binding, "10.20.30.40");
    EXPECT_EQ(resolution.outcome, KdbxResolutionOutcome::kNotFound);
}

}  // namespace
}  // namespace openscrt::credential::kdbx
