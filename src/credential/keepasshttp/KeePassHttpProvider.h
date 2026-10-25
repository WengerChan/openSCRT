// The KeePassHTTP credential provider (P2-T8).
//
// The other half of the "dual mode" of the roadmap: the local .kdbx reader is one
// provider, and this one asks a running KeePass instead. The session layer above them
// asks for a credential and never learns where it came from.
//
// The lookup blocks, like every lookup in this layer: the plugin is a local process, the
// request is one round trip, and the interface says that the caller runs it on a worker
// thread.

#ifndef OPENCRT_CREDENTIAL_KEEPASSHTTP_KEEPASSHTTP_PROVIDER_H_
#define OPENCRT_CREDENTIAL_KEEPASSHTTP_KEEPASSHTTP_PROVIDER_H_

#include <memory>
#include <string>
#include <string_view>

#include "credential/ICredentialProvider.h"
#include "credential/keepasshttp/KeePassHttpClient.h"

namespace openscrt::credential::keepasshttp {

class KeePassHttpProvider final : public ICredentialProvider {
 public:
    KeePassHttpProvider(std::shared_ptr<IHttpTransport> transport, Association association);

    [[nodiscard]] std::string_view id() const noexcept override { return "keepasshttp"; }

    // Whether the provider could answer at all: an association that was never made is
    // not an association that works.
    [[nodiscard]] bool isAvailable() const noexcept override;

    // Asks the plugin for the entries of the host and takes the first one.
    [[nodiscard]] CredentialResult lookup(const CredentialRequest& request) override;

    // The association, so that the caller can store it once the user has confirmed it
    // in KeePass.
    [[nodiscard]] const Association& association() const noexcept { return association_; }

    // Replaces the association, e.g. after a new pairing.
    void setAssociation(Association association) { association_ = std::move(association); }

 private:
    KeePassHttpClient client_;
    Association association_;
};

}  // namespace openscrt::credential::keepasshttp

#endif  // OPENCRT_CREDENTIAL_KEEPASSHTTP_KEEPASSHTTP_PROVIDER_H_
