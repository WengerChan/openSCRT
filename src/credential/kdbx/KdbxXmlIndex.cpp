#include "credential/kdbx/KdbxXmlIndex.h"

#include <openssl/evp.h>
#include <pugixml.hpp>
#include <sodium.h>

#include <cstring>

namespace openscrt::credential::kdbx {
namespace {

// The keys of the fields this index cares about.
constexpr const char* kKeyTitle = "Title";
constexpr const char* kKeyUserName = "UserName";
constexpr const char* kKeyPassword = "Password";
constexpr const char* kKeyUrl = "URL";

std::string childText(const pugi::xml_node& parent, const char* name) {
    const pugi::xml_node node = parent.child(name);
    return node ? std::string(node.child_value()) : std::string();
}

// The base64 decoder of OpenSSL, without the line breaks base64 usually carries.
bool decodeBase64(const std::string& text, std::vector<std::uint8_t>& out) {
    out.clear();
    if (text.empty()) {
        return true;
    }
    std::string compact;
    compact.reserve(text.size());
    for (const char character : text) {
        if (character != '\n' && character != '\r' && character != ' ' && character != '\t') {
            compact.push_back(character);
        }
    }
    if (compact.size() % 4 != 0) {
        return false;
    }
    out.resize((compact.size() / 4) * 3);
    const int written =
        EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(compact.data()),
                        static_cast<int>(compact.size()));
    if (written < 0) {
        out.clear();
        return false;
    }
    // EVP_DecodeBlock does not strip the padding itself.
    std::size_t padding = 0;
    if (!compact.empty() && compact.back() == '=') {
        padding = 1;
        if (compact.size() > 1 && compact[compact.size() - 2] == '=') {
            padding = 2;
        }
    }
    out.resize(static_cast<std::size_t>(written) - padding);
    return true;
}

// Walks the groups, collecting the entries.
void collectEntries(const pugi::xml_node& group, const std::string& parent_path, KdbxIndex& index,
                    std::string& error, bool& failed) {
    if (failed) {
        return;
    }
    const std::string name = childText(group, "Name");
    const std::string path = parent_path.empty() ? name : parent_path + "/" + name;

    const pugi::xml_node entries = group.child("Entries");
    for (pugi::xml_node entry = entries.child("Entry"); entry;
         entry = entry.next_sibling("Entry")) {
        KdbxEntry record;
        record.uuid = childText(entry, "UUID");
        record.group_path = path;

        for (pugi::xml_node field = entry.child("String"); field;
             field = field.next_sibling("String")) {
            const std::string key = childText(field, "Key");
            const pugi::xml_node value = field.child("Value");
            if (!value) {
                continue;
            }
            const std::string text = value.child_value();
            if (key == kKeyTitle) {
                record.title = text;
            } else if (key == kKeyUserName) {
                record.username = text;
            } else if (key == kKeyUrl) {
                record.url = text;
            } else if (key == kKeyPassword) {
                record.password = text;
                record.password_is_protected =
                    std::strcmp(value.attribute("Protected").as_string(), "True") == 0;
            } else {
                record.custom_fields.emplace_back(key, text);
            }
        }
        index.entries.push_back(std::move(record));
    }

    for (pugi::xml_node child = group.child("Group"); child; child = child.next_sibling("Group")) {
        collectEntries(child, path, index, error, failed);
        if (failed) {
            return;
        }
    }
}

}  // namespace

KdbxIndexResult indexKdbxXml(const std::uint8_t* xml, std::size_t size) {
    KdbxIndexResult result;
    if (xml == nullptr || size == 0) {
        result.error = "the database has no XML";
        return result;
    }

    pugi::xml_document document;
    const pugi::xml_parse_result parsed = document.load_buffer(xml, size);
    if (!parsed) {
        result.error = std::string("the database's XML does not parse: ") + parsed.description();
        return result;
    }

    const pugi::xml_node root = document.child("KeePassFile").child("Root");
    if (!root) {
        result.error = "the database's XML has no Root";
        return result;
    }

    bool failed = false;
    std::string error;
    const pugi::xml_node root_group = root.child("Group");
    result.index.root_group_uuid = root_group ? childText(root_group, "UUID") : std::string();
    collectEntries(root_group, std::string(), result.index, error, failed);
    if (failed) {
        result.error = error;
        return result;
    }
    return result;
}

bool revealKdbxPassword(const KdbxEntry& entry, const KdbxProtectedStream& stream,
                        core::SecureByteArray& out, std::string& error) {
    if (entry.password.empty()) {
        error = "the entry has no password";
        return false;
    }
    if (!entry.password_is_protected) {
        // A database that stores the password in the clear: hand it over as it is.
        out.assign(entry.password.data(), entry.password.size());
        return true;
    }

    std::vector<std::uint8_t> ciphertext;
    if (!decodeBase64(entry.password, ciphertext)) {
        error = "the protected password is not valid base64";
        return false;
    }
    std::vector<std::uint8_t> plaintext;
    if (!stream.decrypt(ciphertext.data(), ciphertext.size(), plaintext, error)) {
        return false;
    }
    out.assign(plaintext.data(), plaintext.size());
    sodium_memzero(plaintext.data(), plaintext.size());
    return true;
}

}  // namespace openscrt::credential::kdbx
