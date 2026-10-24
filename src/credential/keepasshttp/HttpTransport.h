// The HTTP transport of the KeePassHTTP client (P2-T8).
//
// The plugin listens on 127.0.0.1:19455 and speaks plain HTTP/1.1 with a JSON body.
// The transport is deliberately synchronous: the credential layer is synchronous too
// (the interface says the lookup blocks and belongs on a worker thread), and a
// synchronous client is what that shape needs.
//
// It is a socket and not a Qt signal-based HTTP stack on purpose: the request is one
// POST to a local process, and doing it by hand keeps the credential layer free of
// an event loop.

#ifndef OPENCRT_CREDENTIAL_KEEPASSHTTP_HTTP_TRANSPORT_H_
#define OPENCRT_CREDENTIAL_KEEPASSHTTP_HTTP_TRANSPORT_H_

#include <QString>
#include <string>

#include "credential/keepasshttp/KeePassHttpClient.h"

namespace openscrt::credential::keepasshttp {

class QtHttpTransport final : public IHttpTransport {
 public:
    explicit QtHttpTransport(QString host = QStringLiteral("127.0.0.1"), quint16 port = 19455,
                             int timeout_ms = 5000);

    [[nodiscard]] bool post(const std::string& body, std::string& response,
                            std::string& error) override;

 private:
    QString host_;
    quint16 port_;
    int timeout_ms_;
};

}  // namespace openscrt::credential::keepasshttp

#endif  // OPENCRT_CREDENTIAL_KEEPASSHTTP_HTTP_TRANSPORT_H_
