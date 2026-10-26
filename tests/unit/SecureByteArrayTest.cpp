// P2-A7 groundwork: SecureByteArray is the only sanctioned home for credential plaintext, so
// its wipe/ownership contract is verified here even though the KDBX reader arrives in P2-T4.

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include "core/SecureByteArray.h"

namespace openscrt::core {
namespace {

// P2-A7: "the locked page count returns to zero after SecureByteArray is destroyed".
//
// The only machine-readable way to observe that is the kernel's own count of locked
// pages for this process, and on Linux that is VmLck in /proc/self/status. Windows and
// macOS do not expose an equivalent counter, so the test skips there instead of
// pretending to have checked something: the other platforms still get the `locked()`
// honesty test below, which covers the degraded path.
#if defined(__linux__)
std::optional<long> lockedKilobytes() {
    std::ifstream status("/proc/self/status");
    if (!status.is_open()) {
        return std::nullopt;
    }
    std::string key;
    while (status >> key) {
        if (key == "VmLck:") {
            long value = 0;
            status >> value;
            return value;
        }
        // Skip the remainder of this line so the next token is a key again.
        std::string rest;
        std::getline(status, rest);
    }
    return std::nullopt;
}
#endif

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
    EXPECT_EQ(secret.copyOut(out, sizeof(out)), 6u);
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

#if defined(__linux__)
TEST(SecureByteArrayTest, TheDestructorGivesTheLockedPagesBack) {
    // P2-A7, first half: the pages the buffer locked must be unlocked when it dies, or
    // every unlocked database would leak locked pages until the process ends.
    const std::optional<long> before = lockedKilobytes();
    if (!before.has_value()) {
        GTEST_SKIP() << "this kernel does not report VmLck";
    }

    bool was_locked = false;
    std::optional<long> during;
    {
        SecureByteArray secret;
        secret.resize(64 * 1024);
        secret.assign("topsecret", 9);
        was_locked = secret.locked();
        during = lockedKilobytes();
    }  // the destructor runs here

    ASSERT_TRUE(during.has_value());
    const std::optional<long> after = lockedKilobytes();
    ASSERT_TRUE(after.has_value());

    // The numbers go to the log on purpose: ctest hides the output of a passing test,
    // so without this line the evidence would be a green tick and nothing else. And if
    // the case skips, the log has to say so out loud rather than let the reader infer
    // that the criterion was checked.
    std::cout << "VmLck before=" << *before << " kB, during=" << *during << " kB, after=" << *after
              << " kB, mlock succeeded=" << (was_locked ? "yes" : "no") << '\n';

    if (!was_locked) {
        GTEST_SKIP() << "mlock is unavailable here (RLIMIT_MEMLOCK); the unlock path cannot "
                        "be observed on this machine";
    }
    EXPECT_GT(*during, *before) << "the pages were locked while the buffer was alive";
    EXPECT_EQ(*after, *before) << "the destructor must unlock the pages it locked";
}
#endif

// The edges of the container that the other tests do not reach: a buffer that is asked
// to hold nothing, and a copy-out into a place that does not exist.
TEST(SecureByteArrayTest, ABufferOfAGivenSizeIsUsable) {
    SecureByteArray buffer(16);
    EXPECT_EQ(buffer.size(), 16u);
    EXPECT_FALSE(buffer.empty());
}

TEST(SecureByteArrayTest, AssigningNothingWipesTheBuffer) {
    SecureByteArray buffer;
    buffer.assign("something", 9);
    ASSERT_EQ(buffer.size(), 9u);

    buffer.assign(nullptr, 4);

    // The bytes are wiped; the length is what it was, because the buffer is still
    // there and only its content is gone.
    EXPECT_EQ(buffer.size(), 9u);
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        EXPECT_EQ(buffer.data()[i], 0u) << "byte " << i << " was not wiped";
    }
}

TEST(SecureByteArrayTest, WithCStringWithoutAnActionIsRefused) {
    SecureByteArray buffer;
    buffer.assign("something", 9);
    EXPECT_FALSE(buffer.withCString(std::function<bool(const char*)>()));
}

TEST(SecureByteArrayTest, WithCStringOfAnEmptyBufferHandsOverAnEmptyString) {
    SecureByteArray buffer;
    bool called = false;
    std::string seen = "not empty";
    const bool result = buffer.withCString([&called, &seen](const char* text) {
        called = true;
        seen = text;
        return true;
    });
    EXPECT_TRUE(result);
    EXPECT_TRUE(called) << "the action must run even for an empty buffer";
    EXPECT_TRUE(seen.empty()) << "the action got something it should not have";
}

TEST(SecureByteArrayTest, CopyingOutIntoNothingCopiesNothing) {
    SecureByteArray buffer;
    buffer.assign("something", 9);

    std::uint8_t destination[4] = {1, 2, 3, 4};
    EXPECT_EQ(buffer.copyOut(nullptr, 4), 0u);
    EXPECT_EQ(buffer.copyOut(destination, 0), 0u);
    EXPECT_EQ(destination[0], 1u) << "nothing was written";
}

}  // namespace
}  // namespace openscrt::core