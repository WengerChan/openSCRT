// P1-T11: the ssh.com ("VanDyke") private key format.
//
// The fixtures are real keys, taken from SSH.NET's test data (Renci.SshNet, MIT licensed):
// one unencrypted and one encrypted with 3DES-CBC under the passphrase "12345". They are
// the same key in two shapes, so parsing both and comparing the fields is a check that
// does not depend on our own expectations.
//
// The expected public key in sshcom_rsa_plain.pub was produced by tools/sshcom_pubkey.py,
// an independent implementation of the same format. If the C++ parser and the Python one
// agree, the parser is not merely agreeing with itself.

#include <gtest/gtest.h>
#include <libssh/libssh.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "protocols/ssh/Ssh2Channel.h"
#include "protocols/ssh/SshComKey.h"

namespace openscrt::protocols::ssh {
namespace {

const char* kPlainKey = OPENCRT_SOURCE_DIR "/tests/fixtures/ssh/sshcom_rsa_plain.key";
const char* kEncryptedKey = OPENCRT_SOURCE_DIR "/tests/fixtures/ssh/sshcom_rsa_des.key";
const char* kPublicKey = OPENCRT_SOURCE_DIR "/tests/fixtures/ssh/sshcom_rsa_plain.pub";
const char* kPassphrase = "12345";

std::string readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string trimmed(const std::string& text) {
    std::size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

}  // namespace

TEST(SshComKeyTest, RecognisesItsOwnHeader) {
    EXPECT_TRUE(isSshComKeyFile(kPlainKey));
    EXPECT_TRUE(isSshComKeyFile(kEncryptedKey));
    EXPECT_FALSE(isSshComKeyFile("/does/not/exist/at/all"));
}

TEST(SshComKeyTest, ReadsAnUnencryptedKey) {
    SshComRsaKey key;
    std::string error;
    ASSERT_TRUE(readSshComKeyFile(kPlainKey, "", key, error)) << error;

    // 2048-bit modulus, exponent 65537.
    EXPECT_EQ(key.modulus.size(), 256u);
    EXPECT_EQ(key.exponent.size(), 3u);
    EXPECT_EQ(key.exponent[2], 0x01u);
    EXPECT_EQ(key.exponent[1], 0x00u);
    EXPECT_EQ(key.exponent[0], 0x01u);
    EXPECT_FALSE(key.private_exponent.empty());
    EXPECT_FALSE(key.p.empty());
    EXPECT_FALSE(key.q.empty());
    EXPECT_FALSE(key.inverse_q.empty());
}

TEST(SshComKeyTest, DecryptsAnEncryptedKeyToTheSameKey) {
    SshComRsaKey plain;
    std::string error;
    ASSERT_TRUE(readSshComKeyFile(kPlainKey, "", plain, error)) << error;

    SshComRsaKey encrypted;
    ASSERT_TRUE(readSshComKeyFile(kEncryptedKey, kPassphrase, encrypted, error)) << error;

    EXPECT_EQ(plain.modulus, encrypted.modulus);
    EXPECT_EQ(plain.exponent, encrypted.exponent);
    EXPECT_EQ(plain.private_exponent, encrypted.private_exponent);
    EXPECT_EQ(plain.p, encrypted.p);
    EXPECT_EQ(plain.q, encrypted.q);
    EXPECT_EQ(plain.inverse_q, encrypted.inverse_q);
}

TEST(SshComKeyTest, RefusesAWrongPassphrase) {
    SshComRsaKey key;
    std::string error;
    EXPECT_FALSE(readSshComKeyFile(kEncryptedKey, "not the passphrase", key, error));
    EXPECT_NE(error.find("passphrase"), std::string::npos) << error;
}

TEST(SshComKeyTest, NeedsAPassphraseForAnEncryptedKey) {
    SshComRsaKey key;
    std::string error;
    EXPECT_FALSE(readSshComKeyFile(kEncryptedKey, "", key, error));
    EXPECT_NE(error.find("passphrase"), std::string::npos) << error;
}

TEST(SshComKeyTest, ConvertsToAKeyLibsshCanImport) {
    SshComRsaKey key;
    std::string error;
    ASSERT_TRUE(readSshComKeyFile(kPlainKey, "", key, error)) << error;

    const std::string openssh = toOpenSshPrivateKey(key);
    ASSERT_NE(openssh.find("-----BEGIN OPENSSH PRIVATE KEY-----"), std::string::npos);
    ASSERT_NE(openssh.find("-----END OPENSSH PRIVATE KEY-----"), std::string::npos);

    ssh_key imported = nullptr;
    ASSERT_EQ(ssh_pki_import_privkey_base64(openssh.c_str(), nullptr, nullptr, nullptr, &imported),
              SSH_OK);
    ASSERT_NE(imported, nullptr);

    // The point of the conversion: libssh can use the key, and the public key it derives
    // from it is the one an independent implementation derived from the same file.
    char* exported = nullptr;
    ASSERT_EQ(ssh_pki_export_pubkey_base64(imported, &exported), SSH_OK);
    ASSERT_NE(exported, nullptr);
    const std::string public_key = exported;
    ssh_string_free_char(exported);
    ssh_key_free(imported);

    EXPECT_EQ(public_key, trimmed(readFile(kPublicKey)));
}

TEST(SshComKeyTest, LogsInWithAnSshComKey) {
    const char* port = std::getenv("OPENCRT_TEST_SSH_PORT");
    const char* user = std::getenv("OPENCRT_TEST_SSH_USER");
    if (port == nullptr || user == nullptr) {
        GTEST_SKIP() << "no SSH server configured for this run";
    }

    // The encrypted key on purpose: it is the harder path, and the server has the public
    // key derived from the same file, so a successful login means the whole chain worked --
    // parse, decrypt, convert, and authenticate.
    SshConnectionOptions options;
    options.host = "127.0.0.1";
    options.port = std::atoi(port);
    options.user = user;
    options.private_key_path = kEncryptedKey;
    options.private_key_passphrase = kPassphrase;
    options.auth_method = SshAuthMethod::kPublicKey;
    options.timeout_seconds = 20;

    Ssh2Channel channel{options};
    const TransportResult result = channel.connectAsync();
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(channel.state(), TransportState::kConnected);
    EXPECT_TRUE(channel.disconnect().ok);
}

}  // namespace openscrt::protocols::ssh
