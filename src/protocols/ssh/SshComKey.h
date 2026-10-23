// The ssh.com ("VanDyke") private key format.
//
// This is the format SecureCRT itself writes, so a user migrating from SecureCRT brings
// keys in this shape. libssh does not read it: it knows PEM and the OpenSSH container and
// nothing else, so a key in this format is read here and converted before libssh sees it.
//
// The layout follows SSH.NET's implementation (Renci.SshNet, MIT licensed), which is the
// clearest public description of the format:
//
//   "---- BEGIN SSH2 ENCRYPTED PRIVATE KEY ----"
//   Comment: "..."
//   <base64 of the body>
//   "---- END SSH2 ENCRYPTED PRIVATE KEY ----"
//
// and the body is
//
//   uint32  magic (0x3f6ff9eb)
//   uint32  total length, including the magic
//   string  key type ("if-modn{sign{rsa-...}" and friends)
//   string  cipher name ("none" or "3des-cbc")
//   uint32  blob length
//   bytes   blob
//
// The blob holds the RSA fields, each prefixed with its bit count, and is encrypted with
// 3DES-CBC when the cipher name says so.

#ifndef OPENCRT_PROTOCOLS_SSH_SSHCOM_KEY_H_
#define OPENCRT_PROTOCOLS_SSH_SSHCOM_KEY_H_

#include <cstdint>
#include <string>
#include <vector>

namespace openscrt::protocols::ssh {

// An RSA key taken from an ssh.com key file. Every number is big-endian and unsigned.
struct SshComRsaKey {
    std::vector<std::uint8_t> modulus;           // n
    std::vector<std::uint8_t> exponent;          // e
    std::vector<std::uint8_t> private_exponent;  // d
    std::vector<std::uint8_t> inverse_q;         // iqmp
    std::vector<std::uint8_t> p;
    std::vector<std::uint8_t> q;
};

// True when the file starts with the ssh.com header. It reads the first line only, so
// it is cheap enough to call before deciding which importer to use.
[[nodiscard]] bool isSshComKeyFile(const std::string& path);

// Reads an ssh.com key file, decrypting it when it is encrypted.
//
// The passphrase is only used for the encrypted variant; an unencrypted key ignores it.
// On failure the reason is in `error` -- "wrong passphrase" is the one a user is most
// likely to hit, and it is worth saying plainly.
[[nodiscard]] bool readSshComKeyFile(const std::string& path, const std::string& passphrase,
                                     SshComRsaKey& key, std::string& error);

// Renders the key as an OpenSSH v1 private key: the PEM header and footer around the
// base64 of the container. That is the format libssh can import, so this is the bridge
// between the two worlds.
[[nodiscard]] std::string toOpenSshPrivateKey(const SshComRsaKey& key);

}  // namespace openscrt::protocols::ssh

#endif  // OPENCRT_PROTOCOLS_SSH_SSHCOM_KEY_H_
