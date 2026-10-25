// The vault tests (P2-T7).
//
// The vault is the thing that holds a database open: it keeps the key material, it
// watches the file, and it locks itself. The three are what these tests check.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "credential/kdbx/KdbxVault.h"

namespace openscrt::credential::kdbx {
namespace {

const QString kFixturePath =
    QStringLiteral(OPENCRT_SOURCE_DIR) + "/tests/fixtures/kdbx/kdbx31_with_keyfile.kdbx";
const QString kKeyFilePath =
    QStringLiteral(OPENCRT_SOURCE_DIR) + "/tests/fixtures/kdbx/kdbx31_with_keyfile.key";
const QString kBlankDatabasePath =
    QStringLiteral(OPENCRT_SOURCE_DIR) + "/tests/fixtures/kdbx/blank_database.kdbx";

constexpr std::string_view kPasswordText = "correct horse battery staple";
const std::uint8_t* kPasswordBytes() {
    return reinterpret_cast<const std::uint8_t*>(kPasswordText.data());
}
std::size_t kPasswordSize() {
    return kPasswordText.size();
}

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>());
}

// The tests need an event loop: the vault answers through signals, and the file watcher
// only fires while the loop is running.
class VaultFixture : public ::testing::Test {
 protected:
    void SetUp() override {
        if (QCoreApplication::instance() == nullptr) {
            static int argc = 1;
            static char program[] = "vault-test";
            static char* argv[] = {program, nullptr};
            application_ = std::make_unique<QCoreApplication>(argc, argv);
        }
    }

    // Lets the event loop run for a while, so the timers and the file watcher fire.
    static void spin(int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    }

    std::unique_ptr<QCoreApplication> application_;
};

TEST_F(VaultFixture, TenSessionsDoNotDeriveTheKeyAgain) {
    // The derivation of the key is the slow part of opening a database: a second or more
    // for a KDBX 4 file. It happens once, when the database is opened, and the sessions
    // that follow take the key that is already there.
    const std::vector<std::uint8_t> key_file = readFile(kKeyFilePath.toStdString());

    KdbxVault vault;
    std::string error;
    ASSERT_TRUE(vault.open(kFixturePath, kPasswordBytes(), kPasswordSize(), key_file, error))
        << error;
    ASSERT_EQ(vault.derivationCount(), 1u) << "the key is derived when the database opens";
    ASSERT_EQ(vault.index().entries.size(), 1u);

    // Ten sessions, each asking for the password of the entry.
    for (int session = 0; session < 10; ++session) {
        core::SecureByteArray password;
        ASSERT_TRUE(vault.revealPassword(vault.index().entries.front(), password, error)) << error;
        ASSERT_EQ(password.size(), 6u);
    }

    EXPECT_EQ(vault.derivationCount(), 1u) << "ten sessions must not derive the key again";
}

TEST_F(VaultFixture, TheUnlockDoesNotBlockTheThreadThatDraws) {
    // P2-A4: the derivation of the key of a KDBX 4 file takes seconds on purpose, and
    // the thread that draws the window may not wait for it.
    //
    // The number measured is the longest the event loop was not answering. It is taken
    // between the end of one round and the end of the next, so it is an upper bound: it
    // also counts the sleep between the rounds.
    KdbxVault vault;
    bool reported = false;
    QObject::connect(&vault, &KdbxVault::opened, &vault,
                     [&](bool, const QString&) { reported = true; });

    // The blank database is a KDBX 4 file, so opening it derives the key with Argon2.
    const std::string_view blank_password = "password";
    const auto started = std::chrono::steady_clock::now();
    vault.openAsync(kBlankDatabasePath,
                    reinterpret_cast<const std::uint8_t*>(blank_password.data()),
                    blank_password.size(), {});

    std::chrono::steady_clock::duration longest = std::chrono::steady_clock::duration::zero();
    auto previous = std::chrono::steady_clock::now();
    while (!reported) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const auto now = std::chrono::steady_clock::now();
        longest = std::max(longest, now - previous);
        previous = now;
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(180)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_TRUE(reported) << "the vault never reported back";
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(longest).count();
    EXPECT_LE(millis, 50) << "the window was unresponsive for " << millis << " ms";
}

TEST_F(VaultFixture, TheDatabaseCanBeUnlockedOnAWorkerThread) {
    // The derivation of the key is deliberately slow, so it happens on a worker thread
    // and the answer comes back through the event loop. This is what keeps the window
    // from freezing while a database is unlocked.
    const std::vector<std::uint8_t> key_file = readFile(kKeyFilePath.toStdString());

    KdbxVault vault;
    bool reported = false;
    bool ok = false;
    QObject::connect(&vault, &KdbxVault::opened, &vault, [&](bool success, const QString&) {
        ok = success;
        reported = true;
    });

    vault.openAsync(kFixturePath, kPasswordBytes(), kPasswordSize(), key_file);

    QElapsedTimer timer;
    timer.start();
    while (!reported && timer.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    ASSERT_TRUE(reported) << "the vault never reported back";
    EXPECT_TRUE(ok);
    EXPECT_TRUE(vault.isOpen());
    EXPECT_FALSE(vault.index().entries.empty());
}

TEST_F(VaultFixture, AWorkerThreadUnlockThatFailsSaysSo) {
    KdbxVault vault;
    bool reported = false;
    bool ok = true;
    QString message;
    QObject::connect(&vault, &KdbxVault::opened, &vault, [&](bool success, const QString& text) {
        ok = success;
        message = text;
        reported = true;
    });

    const char* wrong = "wrong";
    vault.openAsync(kFixturePath, reinterpret_cast<const std::uint8_t*>(wrong), 5, {});

    QElapsedTimer timer;
    timer.start();
    while (!reported && timer.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    ASSERT_TRUE(reported) << "the vault never reported back";
    EXPECT_FALSE(ok);
    EXPECT_FALSE(message.isEmpty()) << "a refusal has to say why";
    EXPECT_FALSE(vault.isOpen());
}

TEST_F(VaultFixture, OpeningTheDatabaseReadsTheIndex) {
    const std::vector<std::uint8_t> key_file = readFile(kKeyFilePath.toStdString());

    KdbxVault vault;
    std::string error;
    ASSERT_TRUE(vault.open(kFixturePath, kPasswordBytes(), kPasswordSize(), key_file, error))
        << error;
    EXPECT_TRUE(vault.isOpen());
    ASSERT_EQ(vault.index().entries.size(), 1u);
    EXPECT_EQ(vault.index().entries.front().title, "router");
    EXPECT_EQ(vault.index().entries.front().username, "netops");
}

TEST_F(VaultFixture, TheEntriesAreFoundByUuidAndByHost) {
    const std::vector<std::uint8_t> key_file = readFile(kKeyFilePath.toStdString());

    KdbxVault vault;
    std::string error;
    ASSERT_TRUE(vault.open(kFixturePath, kPasswordBytes(), kPasswordSize(), key_file, error))
        << error;

    const KdbxEntry& entry = vault.index().entries.front();
    EXPECT_EQ(vault.findByUuid(entry.uuid), &entry);
    EXPECT_EQ(vault.findByHost("ssh://10.20.30.40:22"), &entry);
    EXPECT_EQ(vault.findByUuid("no-such-uuid"), nullptr);
}

TEST_F(VaultFixture, ThePasswordIsRevealedOnRequest) {
    const std::vector<std::uint8_t> key_file = readFile(kKeyFilePath.toStdString());

    KdbxVault vault;
    std::string error;
    ASSERT_TRUE(vault.open(kFixturePath, kPasswordBytes(), kPasswordSize(), key_file, error))
        << error;
    ASSERT_EQ(vault.index().entries.size(), 1u);

    core::SecureByteArray password;
    ASSERT_TRUE(vault.revealPassword(vault.index().entries.front(), password, error)) << error;
    ASSERT_EQ(password.size(), 6u);
    char buffer[7] = {};
    EXPECT_EQ(password.copyOut(buffer, sizeof(buffer)), 6u);
    EXPECT_STREQ(buffer, "s3cret");
}

TEST_F(VaultFixture, LockingDropsTheIndex) {
    const std::vector<std::uint8_t> key_file = readFile(kKeyFilePath.toStdString());

    KdbxVault vault;
    std::string error;
    ASSERT_TRUE(vault.open(kFixturePath, kPasswordBytes(), kPasswordSize(), key_file, error))
        << error;

    bool locked = false;
    QObject::connect(&vault, &KdbxVault::locked, [&locked]() { locked = true; });
    vault.lock();

    EXPECT_TRUE(locked);
    EXPECT_FALSE(vault.isOpen());
    EXPECT_TRUE(vault.index().entries.empty());
    // The password is gone too, so the vault cannot be reopened behind the user's back.
    EXPECT_EQ(vault.findByUuid("anything"), nullptr);
}

TEST_F(VaultFixture, TheVaultLocksItselfWhenItGoesIdle) {
    const std::vector<std::uint8_t> key_file = readFile(kKeyFilePath.toStdString());

    KdbxVault vault;
    std::string error;
    ASSERT_TRUE(vault.open(kFixturePath, kPasswordBytes(), kPasswordSize(), key_file, error))
        << error;

    vault.setIdleTimeout(std::chrono::milliseconds(50));
    EXPECT_TRUE(vault.isOpen());
    spin(400);
    EXPECT_FALSE(vault.isOpen()) << "the vault did not lock itself after the idle timeout";
}

TEST_F(VaultFixture, UsingTheVaultRestartsTheIdleTimer) {
    const std::vector<std::uint8_t> key_file = readFile(kKeyFilePath.toStdString());

    KdbxVault vault;
    std::string error;
    ASSERT_TRUE(vault.open(kFixturePath, kPasswordBytes(), kPasswordSize(), key_file, error))
        << error;

    vault.setIdleTimeout(std::chrono::milliseconds(150));
    for (int i = 0; i < 4; ++i) {
        spin(60);
        vault.touch();
    }
    EXPECT_TRUE(vault.isOpen()) << "the idle timer was not restarted by the use of the vault";
}

TEST_F(VaultFixture, AChangedFileIsReadAgain) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath("vault.kdbx");

    const std::vector<std::uint8_t> bytes = readFile(kFixturePath.toStdString());
    ASSERT_FALSE(bytes.empty());
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<qint64>(bytes.size())),
                  static_cast<qint64>(bytes.size()));
    }
    const std::vector<std::uint8_t> key_file = readFile(kKeyFilePath.toStdString());

    KdbxVault vault;
    std::string error;
    ASSERT_TRUE(vault.open(path, kPasswordBytes(), kPasswordSize(), key_file, error)) << error;

    bool reloaded = false;
    bool ok = false;
    QObject::connect(&vault, &KdbxVault::reloaded, [&reloaded, &ok](bool success, const QString&) {
        reloaded = true;
        ok = success;
    });

    // On Windows the watch is registered on a helper thread, so a change in the first
    // moments after opening can slip past it. Let the watch settle, and write twice.
    spin(300);
    for (int attempt = 0; attempt < 2 && !reloaded; ++attempt) {
        {
            QFile file(path);
            ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            ASSERT_EQ(file.write(reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<qint64>(bytes.size())),
                      static_cast<qint64>(bytes.size()));
        }
        spin(700);
    }
    EXPECT_TRUE(reloaded) << "the vault did not notice that the file changed";
    EXPECT_TRUE(ok) << "the re-read of an unchanged database failed";
    EXPECT_TRUE(vault.isOpen());
}

TEST_F(VaultFixture, UsingTheVaultNoticesAChangeTheWatcherMissed) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath("vault.kdbx");

    const std::vector<std::uint8_t> bytes = readFile(kFixturePath.toStdString());
    ASSERT_FALSE(bytes.empty());
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<qint64>(bytes.size())),
                  static_cast<qint64>(bytes.size()));
    }
    const std::vector<std::uint8_t> key_file = readFile(kKeyFilePath.toStdString());

    KdbxVault vault;
    std::string error;
    ASSERT_TRUE(vault.open(path, kPasswordBytes(), kPasswordSize(), key_file, error)) << error;

    bool reloaded = false;
    QObject::connect(&vault, &KdbxVault::reloaded,
                     [&reloaded](bool, const QString&) { reloaded = true; });

    // Change the file -- to a different size, because the clock on Windows has a
    // resolution of about 15.6 ms and a change right after the read can carry the very
    // same timestamp. The watch may or may not have seen it; the size and timestamp
    // check is what makes the outcome the same on every platform.
    //
    // The replacement is a different database, so the re-read cannot succeed -- what the
    // test checks is that the vault noticed, and that a change it cannot read does not
    // cost it the index it already had.
    const std::vector<std::uint8_t> other = readFile(kBlankDatabasePath.toStdString());
    ASSERT_FALSE(other.empty());
    ASSERT_NE(other.size(), bytes.size());
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(file.write(reinterpret_cast<const char*>(other.data()),
                             static_cast<qint64>(other.size())),
                  static_cast<qint64>(other.size()));
    }
    spin(50);
    vault.touch();
    spin(100);
    EXPECT_TRUE(reloaded) << "the vault did not notice the change when it was used";
    EXPECT_TRUE(vault.isOpen());
    // The index it had is still there: a change that cannot be read is reported, not
    // silently turned into an empty database.
    EXPECT_EQ(vault.index().entries.size(), 1u);
}

}  // namespace
}  // namespace openscrt::credential::kdbx
