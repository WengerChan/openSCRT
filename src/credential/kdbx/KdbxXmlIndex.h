// The XML index of a KeePass database (P2-T6).
//
// The database's XML is a tree of groups and entries. This turns it into a flat list
// of entries that the session layer can look things up in, and -- this is the point --
// keeps the passwords protected: an entry's password stays in the form the database
// stored it, and only becomes plaintext when a session actually asks for it. The
// roadmap asks for exactly that ("save its offset and the inner random stream state,
// and restore the plaintext in locked memory at the moment the SSH connection is
// made"), and the alternative -- decrypting every password when the file is opened --
// would put every secret of the database in memory at once.

#ifndef OPENCRT_CREDENTIAL_KDBX_KDBX_XML_INDEX_H_
#define OPENCRT_CREDENTIAL_KDBX_KDBX_XML_INDEX_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/SecureByteArray.h"
#include "credential/kdbx/KdbxProtectedStream.h"

namespace openscrt::credential::kdbx {

// One entry of the database.
struct KdbxEntry {
    // The uuid as the format stores it: base64.
    std::string uuid;
    // The path of the group the entry is in, e.g. "Root/Homebanking".
    std::string group_path;
    std::string title;
    std::string username;
    std::string url;

    // The password as it is stored: base64 of the encrypted value when the entry is
    // protected, the plaintext otherwise. It is never decrypted on the way in.
    std::string password;
    bool password_is_protected{false};

    // The custom fields of the entry, e.g. "SSH Passphrase" or "TOTP Seed".
    std::vector<std::pair<std::string, std::string>> custom_fields;
};

struct KdbxIndex {
    std::vector<KdbxEntry> entries;
    std::string root_group_uuid;
};

struct KdbxIndexResult {
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    std::string error;
    KdbxIndex index;
};

// Parses the XML of a database.
//
// The XML is not trusted: a database is a file somebody handed us, and a malformed or
// hostile one must not be able to make the reader allocate without bound.
[[nodiscard]] KdbxIndexResult indexKdbxXml(const std::uint8_t* xml, std::size_t size);

// Turns an entry's password into plaintext, in the caller's locked buffer.
//
// The caller owns the buffer and wipes it when the connection attempt is over; nothing
// here keeps a copy.
[[nodiscard]] bool revealKdbxPassword(const KdbxEntry& entry, const KdbxProtectedStream& stream,
                                      core::SecureByteArray& out, std::string& error);

}  // namespace openscrt::credential::kdbx

#endif  // OPENCRT_CREDENTIAL_KDBX_KDBX_XML_INDEX_H_
