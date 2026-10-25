#include "credential/keepasshttp/KeePassHttpAssociation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace openscrt::credential::keepasshttp {
namespace {

// The two halves of the association, one after the other, separated by a line break.
//
// Neither the id nor the key ever holds a line break: the id is assigned by the plugin
// and the key is base64.
std::vector<std::uint8_t> serialize(const Association& association) {
    const std::string text = association.key_id + "\n" + association.key_base64;
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

bool deserialize(const std::vector<std::uint8_t>& bytes, Association& association,
                 std::string& error) {
    const std::string text(bytes.begin(), bytes.end());
    const std::size_t separator = text.find('\n');
    if (separator == std::string::npos) {
        error = "the stored association is not one this build wrote";
        return false;
    }
    association.key_id = text.substr(0, separator);
    association.key_base64 = text.substr(separator + 1);
    if (association.key_id.empty() || association.key_base64.empty()) {
        error = "the stored association is not one this build wrote";
        return false;
    }
    return true;
}

}  // namespace

bool saveAssociation(ISecretStore& store, const Association& association, std::string& error) {
    return store.store(kAssociationName, serialize(association), error);
}

bool loadAssociation(ISecretStore& store, Association& association, std::string& error) {
    std::vector<std::uint8_t> bytes;
    if (!store.load(kAssociationName, bytes, error)) {
        return false;
    }
    return deserialize(bytes, association, error);
}

bool forgetAssociation(ISecretStore& store, std::string& error) {
    return store.remove(kAssociationName, error);
}

}  // namespace openscrt::credential::keepasshttp
