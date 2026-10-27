// The SSH port forwarders (P3-T8).
//
// Three ways of reaching something through an SSH connection:
//
//   * local    -- a port on this machine forwards to a place the server can reach;
//   * remote   -- a port on the server forwards back to this machine;
//   * dynamic  -- a SOCKS proxy on this machine: the client says where it wants to
//                 go and the server is asked to go there.
//
// All three are the same thing underneath: a stream of bytes that has to travel
// through the connection in both directions. What differs is who listens and who
// dials.
//
// The forwarder opens the channels on the session that is already connected, so there
// is no second password and no second handshake.

#ifndef OPENCRT_PROTOCOLS_SSH_SSH_PORT_FORWARDER_H_
#define OPENCRT_PROTOCOLS_SSH_SSH_PORT_FORWARDER_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "protocols/ssh/SshNativeSession.h"

class QTcpServer;
class QTcpSocket;

namespace openscrt::protocols::ssh {

enum class ForwardMode {
    // A port on this machine forwards to a place the server can reach.
    kLocal,
    // A port on the server forwards back to this machine.
    kRemote,
    // A SOCKS proxy on this machine: the client says where it wants to go.
    kDynamic,
};

struct ForwardRule {
    ForwardMode mode{ForwardMode::kLocal};

    // Where the listener is. For a local or a dynamic forward that is this machine;
    // for a remote forward that is the server.
    std::string listen_address{"127.0.0.1"};
    int listen_port{0};

    // Where the traffic goes. For a local forward that is a place the server can
    // reach; for a dynamic forward it comes from the client.
    std::string remote_address;
    int remote_port{0};
};

class SshPortForwarder {
 public:
    SshPortForwarder(SshNativeSession session, ForwardRule rule);
    ~SshPortForwarder();

    SshPortForwarder(const SshPortForwarder&) = delete;
    SshPortForwarder& operator=(const SshPortForwarder&) = delete;

    // Starts the forwarder. The port it listens on is in boundPort() afterwards: a
    // request for port 0 means "any free port".
    bool start(std::string& error);
    void stop();

    // The port the forwarder listens on. For a remote forward that is the port on the
    // server.
    [[nodiscard]] int boundPort() const noexcept { return bound_port_.load(); }

    [[nodiscard]] const ForwardRule& rule() const noexcept { return rule_; }

 private:
    // One connection that is being carried.
    struct Connection {
        QTcpSocket* socket{nullptr};
        void* channel{nullptr};
        std::string target_host;
        int target_port{0};
    };

    void run();
    // One turn of a connection. Returns false when the connection is over.
    [[nodiscard]] bool pump(Connection& connection);
    [[nodiscard]] bool acceptPending();
    // Takes the connection the server asks for when someone connects to a port it is
    // listening on, and opens the other end of it.
    [[nodiscard]] bool acceptRemote();
    // Reads the SOCKS5 request of a dynamic forward and says where it wants to go.
    [[nodiscard]] bool readSocksRequest(Connection& connection, std::string& error);
    void writeSocksReply(Connection& connection, std::uint8_t code);

    SshNativeSession session_;
    ForwardRule rule_;
    std::atomic<int> bound_port_{0};
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    std::unique_ptr<QTcpServer> server_;
    std::vector<Connection> connections_;
};

}  // namespace openscrt::protocols::ssh

#endif  // OPENCRT_PROTOCOLS_SSH_SSH_PORT_FORWARDER_H_
