// The association of the client with the plugin, and where it is kept (P2-T8).
//
// The association holds the shared key of the AES session with the KeePassHttp plugin.
// That key is a secret: anybody who has it can ask the plugin for passwords. So it goes
// into the secret store of the system (the Credential Manager, the Keychain) and not
// into a file of ours.
//
// The two halves of the association are stored as one blob. The id is not a secret on
// its own, but keeping the two together means there is one thing to store and one thing
// to forget.

#ifndef OPENCRT_CREDENTIAL_KEEPASSHTTP_KEEPASSHTTP_ASSOCIATION_H_
#define OPENCRT_CREDENTIAL_KEEPASSHTTP_KEEPASSHTTP_ASSOCIATION_H_

#include <string>

#include "credential/SecretStore.h"
#include "credential/keepasshttp/KeePassHttpClient.h"

namespace openscrt::credential::keepasshttp {

// The name the association is stored under.
inline constexpr const char* kAssociationName = "keePassHttp/association";

// Writes the association into the store.
[[nodiscard]] bool saveAssociation(ISecretStore& store, const Association& association,
                                   std::string& error);

// Reads it back.
[[nodiscard]] bool loadAssociation(ISecretStore& store, Association& association,
                                   std::string& error);

// Removes it. The client is not associated with the plugin any more afterwards.
[[nodiscard]] bool forgetAssociation(ISecretStore& store, std::string& error);

}  // namespace openscrt::credential::keepasshttp

#endif  // OPENCRT_CREDENTIAL_KEEPASSHTTP_KEEPASSHTTP_ASSOCIATION_H_
