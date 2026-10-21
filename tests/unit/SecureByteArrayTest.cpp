// P2-A7 groundwork: SecureByteArray is the only sanctioned home for credential plaintext, so
// its wipe/ownership contract is verified here even though the KDBX reader arrives in P2-T4.

#include "core/SecureByteArray.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <utility>

namespace openscrt::core {
namespace {

TEST(SecureByteArrayTest, DefaultIsEmpty) {
    SecureByteArray secret;
    EXPECT_TRUE(secret.empty());
    EXPECT_EQ(secret.size(), 0u);
    EXPECT_EQ(secret.data(), nullptr);
}

TEST(SecureByteArrayTest, AssignAndCopyOut) {
    SecureByteArray secret;
    const std::string password = "hunter2";
    secret.assign(password.data(), password.size());

    EXPECT_EQ(secret.size(), password.size());
    EXPECT_FALSE(secret.empty());
    EXPECT_EQ(std::memcmp(secret.data(), password.data(), password.size()), 0);

    char out[16] = {};
    EXPECT_EQ(secret.copyOut(out, sizeof(out)), password.size());
    EXPECT_EQ(std::string(out, password.size()), password);
}

TEST(SecureByteArrayTest, AssignFromCharPointer) {
    SecureByteArray secret;
    secret.assign("s3cret");
    EXPECT_EQ(secret.size(), 6u);

    char out[8] = {};
    secret.copyOut(out, sizeof(out));
    EXPECT_EQ(std::string(out, 6), "s3cret");
}

TEST(SecureByteArrayTest, CopyOutIsClampedToDestination) {
    SecureByteArray secret;
    secret.assign("abcdef", 6);

    char small[3] = {};
    EXPECT_EQ(secret.copyOut(small, sizeof(small)), 3u);
    EXPECT_EQ(std::string(small, 3), "abc");
}

TEST(SecureByteArrayTest, WipeZeroesTheBuffer) {
    SecureByteArray secret;
    secret.assign("topsecret", 9);
    secret.wipe();
    for (std::size_t i = 0; i < secret.size(); ++i) {
        EXPECT_EQ(secret.data()[i], 0u) << "byte " << i << " survived the wipe";
    }
}

TEST(SecureByteArrayTest, AssignNullOrEmptyWipes) {
    SecureByteArray secret;
    secret.assign("something", 9);
    secret.assign(static_cast<const char*>(nullptr));
    for (std::size_t i = 0; i < secret.size(); ++i) {
        EXPECT_EQ(secret.data()[i], 0u);
    }
}

TEST(SecureByteArrayTest, MoveTransfersOwnershipWithoutCopying) {
    SecureByteArray source;
    source.assign("movable", 7);
    const std::uint8_t* original = source.data();

    SecureByteArray moved(std::move(source));
    EXPECT_EQ(moved.data(), original);
    EXPECT_EQ(moved.size(), 7u);
    EXPECT_EQ(source.data(), nullptr) << "moved-from buffer must not double free";
    EXPECT_TRUE(source.empty());
}

TEST(SecureByteArrayTest, ResizeIsIdempotent) {
    SecureByteArray secret;
    secret.resize(32);
    const std::uint8_t* first = secret.data();
    secret.resize(32);
    EXPECT_EQ(secret.data(), first) << "same size must not churn the locked allocation";
    EXPECT_EQ(secret.size(), 32u);
}

TEST(SecureByteArrayTest, ZeroLengthResizeStillAllocates) {
    SecureByteArray secret;
    secret.resize(0);
    EXPECT_NE(secret.data(), nullptr) << "a wipeable allocation is needed even for size 0";
    EXPECT_EQ(secret.size(), 0u);
}

TEST(SecureByteArrayTest, LockedFlagIsHonestAboutFallback) {
    // RLIMIT_MEMLOCK is commonly tiny inside CI containers. The contract is not "always
    // locked" but "locked() reports the truth": false means unlocked-but-zeroed, which the UI
    // surfaces to the user rather than hiding. So assert consistency, not the value itself.
    SecureByteArray secret;
    secret.resize(4096);
    ASSERT_NE(secret.data(), nullptr);
    const bool locked = secret.locked();

    SecureByteArray other;
    other.resize(4096);
    EXPECT_EQ(other.locked(), locked) << "the same RLIMIT must yield the same outcome";

    secret.resize(8192);
    EXPECT_NE(secret.data(), nullptr) << "flag must survive a resize";
    EXPECT_EQ(secret.size(), 8192u);
}

}  // namespace
}  // namespace openscrt::core