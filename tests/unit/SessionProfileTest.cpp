// Session profile tests (P1-T12).
//
// The inheritance rules are the substantive part of the roadmap's "三级配置继承树": a
// session stores only what it overrides, and the Default Session template supplies the
// rest. These run without Qt or a display.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "config/SessionProfile.h"

namespace openscrt::config {
namespace {

SessionProfile profile(std::string id, std::string parent = {}) {
    SessionProfile value;
    value.id = std::move(id);
    value.name = value.id;
    value.parent_id = std::move(parent);
    return value;
}

class SessionTreeFixture : public ::testing::Test {
 protected:
    void SetUp() override {
        SessionProfile fallback;
        fallback.name = "Default Session";
        fallback.host = std::string("default.example");
        fallback.port = 2222;
        fallback.user = std::string("guest");
        fallback.protocol = Protocol::kSsh2;
        tree_ = std::make_unique<SessionTree>(fallback);
    }

    std::unique_ptr<SessionTree> tree_;
};

TEST_F(SessionTreeFixture, UnknownIdDoesNotResolve) {
    SessionTree tree;
    SessionTree::Resolved resolved;
    EXPECT_FALSE(tree.resolve("nope", resolved));
}

TEST_F(SessionTreeFixture, ABareSessionInheritsTheDefaultTemplate) {
    ASSERT_TRUE(tree_->add(profile("alpha")));
    SessionTree::Resolved resolved;
    ASSERT_TRUE(tree_->resolve("alpha", resolved));
    EXPECT_EQ(resolved.host, "default.example");
    EXPECT_EQ(resolved.port, 2222);
    EXPECT_EQ(resolved.user, "guest");
    EXPECT_EQ(resolved.protocol, Protocol::kSsh2);
}

TEST_F(SessionTreeFixture, ACloserAncestorWinsOverTheTemplate) {
    auto group = profile("group");
    group.host = std::string("group.example");
    ASSERT_TRUE(tree_->add(std::move(group)));

    auto leaf = profile("leaf", "group");
    leaf.port = 2200;
    ASSERT_TRUE(tree_->add(std::move(leaf)));

    SessionTree::Resolved resolved;
    ASSERT_TRUE(tree_->resolve("leaf", resolved));
    EXPECT_EQ(resolved.host, "group.example") << "inherited from the parent";
    EXPECT_EQ(resolved.port, 2200) << "the session's own value wins";
    EXPECT_EQ(resolved.user, "guest") << "still from the template";
}

TEST_F(SessionTreeFixture, TheNearestOverrideWinsAcrossThreeLevels) {
    auto top = profile("top");
    top.host = std::string("top.example");
    ASSERT_TRUE(tree_->add(std::move(top)));

    auto middle = profile("middle", "top");
    middle.host = std::string("middle.example");
    ASSERT_TRUE(tree_->add(std::move(middle)));

    auto bottom = profile("bottom", "middle");
    bottom.host = std::string("bottom.example");
    ASSERT_TRUE(tree_->add(std::move(bottom)));

    SessionTree::Resolved resolved;
    ASSERT_TRUE(tree_->resolve("bottom", resolved));
    EXPECT_EQ(resolved.host, "bottom.example");
}

TEST_F(SessionTreeFixture, AnIntermediateValueDoesNotEraseAMoreDistantOne) {
    auto top = profile("top");
    top.host = std::string("top.example");
    ASSERT_TRUE(tree_->add(std::move(top)));

    auto middle = profile("middle", "top");
    middle.user = std::string("ops");
    ASSERT_TRUE(tree_->add(std::move(middle)));

    auto bottom = profile("bottom", "middle");
    ASSERT_TRUE(tree_->add(std::move(bottom)));

    SessionTree::Resolved resolved;
    ASSERT_TRUE(tree_->resolve("bottom", resolved));
    EXPECT_EQ(resolved.host, "top.example") << "the middle does not clear what it inherits";
    EXPECT_EQ(resolved.user, "ops");
}

TEST_F(SessionTreeFixture, RejectsEmptyAndDuplicateIds) {
    std::string error;
    EXPECT_FALSE(tree_->add(profile(""), &error));
    ASSERT_TRUE(tree_->add(profile("dup")));
    EXPECT_FALSE(tree_->add(profile("dup"), &error));
    EXPECT_EQ(error, "duplicate session id: dup");
}

TEST_F(SessionTreeFixture, RejectsAnUnknownParent) {
    std::string error;
    EXPECT_FALSE(tree_->add(profile("child", "ghost"), &error));
    EXPECT_EQ(error, "unknown parent: ghost");
}

TEST_F(SessionTreeFixture, RejectsAParentThatWouldFormACycle) {
    ASSERT_TRUE(tree_->add(profile("a")));
    ASSERT_TRUE(tree_->add(profile("b", "a")));
    // Re-parenting "a" under its own descendant would close the loop.
    SessionProfile cyclic = profile("a", "b");
    std::string error;
    EXPECT_FALSE(tree_->add(cyclic, &error));
}

TEST_F(SessionTreeFixture, RefusesToRemoveAParentWithChildren) {
    ASSERT_TRUE(tree_->add(profile("parent")));
    ASSERT_TRUE(tree_->add(profile("child", "parent")));
    EXPECT_FALSE(tree_->remove("parent")) << "orphaning children would silently change them";
    EXPECT_TRUE(tree_->remove("child"));
    EXPECT_TRUE(tree_->remove("parent"));
}

TEST_F(SessionTreeFixture, ChildrenAreListedInInsertionOrder) {
    ASSERT_TRUE(tree_->add(profile("parent")));
    ASSERT_TRUE(tree_->add(profile("one", "parent")));
    ASSERT_TRUE(tree_->add(profile("two", "parent")));

    const auto children = tree_->childrenOf("parent");
    ASSERT_EQ(children.size(), 2u);
    EXPECT_EQ(children[0]->id, "one");
    EXPECT_EQ(children[1]->id, "two");
}

TEST_F(SessionTreeFixture, InheritingEverythingBelowResetsOverrides) {
    ASSERT_TRUE(tree_->add(profile("group")));
    auto child = profile("child", "group");
    child.host = std::string("child.example");
    child.port = 2022;
    ASSERT_TRUE(tree_->add(std::move(child)));
    auto other = profile("other");
    other.host = std::string("other.example");
    ASSERT_TRUE(tree_->add(std::move(other)));

    tree_->inheritEverythingBelow("group");

    EXPECT_TRUE(tree_->find("child")->inheritsEverything());
    EXPECT_FALSE(tree_->find("other")->inheritsEverything()) << "unrelated sessions are untouched";

    SessionTree::Resolved resolved;
    ASSERT_TRUE(tree_->resolve("child", resolved));
    EXPECT_EQ(resolved.host, "default.example") << "back to the template";
    EXPECT_EQ(resolved.port, 2222);
}

TEST_F(SessionTreeFixture, InheritingEverythingBelowAlsoReachesGrandchildren) {
    ASSERT_TRUE(tree_->add(profile("top")));
    ASSERT_TRUE(tree_->add(profile("mid", "top")));
    auto leaf = profile("leaf", "mid");
    leaf.host = std::string("leaf.example");
    ASSERT_TRUE(tree_->add(std::move(leaf)));

    tree_->inheritEverythingBelow("top");
    EXPECT_TRUE(tree_->find("leaf")->inheritsEverything());
}

TEST_F(SessionTreeFixture, EditingTheDefaultTemplateMovesEverythingThatDidNotOverride) {
    ASSERT_TRUE(tree_->add(profile("alpha")));
    auto pinned = profile("beta");
    pinned.host = std::string("pinned.example");
    ASSERT_TRUE(tree_->add(std::move(pinned)));

    SessionProfile updated;
    updated.name = "Default Session";
    updated.host = std::string("new-default.example");
    updated.port = 2200;
    updated.user = std::string("someone");
    updated.protocol = Protocol::kTelnet;
    tree_->setDefaultSession(updated);

    SessionTree::Resolved alpha;
    ASSERT_TRUE(tree_->resolve("alpha", alpha));
    EXPECT_EQ(alpha.host, "new-default.example");
    EXPECT_EQ(alpha.port, 2200);
    EXPECT_EQ(alpha.protocol, Protocol::kTelnet);

    SessionTree::Resolved beta;
    ASSERT_TRUE(tree_->resolve("beta", beta));
    EXPECT_EQ(beta.host, "pinned.example") << "an explicit override survives";
    EXPECT_EQ(beta.port, 2200) << "unset fields still follow the template";
}

TEST(SessionProfileTest, ProtocolRoundTripsThroughText) {
    for (const Protocol protocol :
         {Protocol::kSsh2, Protocol::kTelnet, Protocol::kSerial, Protocol::kLocalShell}) {
        const auto parsed = protocolFromString(toString(protocol));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, protocol);
    }
    EXPECT_FALSE(protocolFromString("Carrier Pigeon").has_value());
}

}  // namespace
}  // namespace openscrt::config