// The jump host proxy (P3-T9).
//
// A machine that cannot be reached directly can be reached through another one: the
// first is the jump host, the second is the target. The client connects to the jump
// host, and the jump host opens the connection to the target on the client's behalf.
//
// That is what the protocol calls a direct-tcpip channel, and libssh opens one with
// the same call that a dynamic forward uses. The difference is who decides where the
// channel goes: the forwarder asks the client, the proxy is told.
//
// More than one level is the same thing twice: a proxy whose target is itself a proxy.

#ifndef OPENCRT_PROTOCOLS_SSH_SSH_JUMP_HOST_PROXY_H_
#define OPENCRT_PROTOCOLS_SSH_SSH_JUMP_HOST_PROXY_H_

#include <memory>
#include <string>
#include <vector>

#include "protocols/ITransportChannel.h"
#include "protocols/ssh/Ssh2Channel.h"
#include "protocols/ssh/SshNativeSession.h"

namespace openscrt::protocols::ssh {

// One hop of the chain.
struct JumpHostStep {
    // The hop itself. An empty host means the target is reached directly.
    std::string host;
    int port{22};
    std::string user;
    std::string private_key_path;
    // The password is never part of a profile: a profile is written to the INI, and a
    // password in the INI is a password on disk.
    core::SecureByteArray password;
};

// A chain of hops and the machine at the end of it.
struct JumpHostChain {
    // The steps before the target. An empty chain means the target is reached
    // directly.
    std::vector<JumpHostStep> hops;
    // The machine at the end of the chain.
    std::string target_host;
    int target_port{22};
};

// Opens a connection to the target through the chain.
//
// The proxy owns the connections to the hops and closes them when it is done. The
// session of the last hop is the one the target is reached through.
class SshJumpHostProxy {
 public:
    explicit SshJumpHostProxy(JumpHostChain chain);
    ~SshJumpHostProxy();

    SshJumpHostProxy(const SshJumpHostProxy&) = delete;
    SshJumpHostProxy& operator=(const SshJumpHostProxy&) = delete;

    // Connects to the target through the chain. The error says which hop failed.
    TransportResult connect();

    // The session of the last hop, which is the one the target is reached through.
    // It is empty when the chain is empty: there is no hop to hold a session, and the
    // target is then reached directly.
    [[nodiscard]] SshNativeSession lastHopSession() const noexcept;

    // The channel that reaches the target. It is the libssh channel, so the type stays
    // behind this header; the caller that wants to talk to the target casts it.
    [[nodiscard]] void* targetChannel() const noexcept { return target_channel_; }

    [[nodiscard]] const JumpHostChain& chain() const noexcept { return chain_; }

 private:
    JumpHostChain chain_;
    // The connection to each hop. The last one is the session the target is reached
    // through.
    std::vector<std::unique_ptr<Ssh2Channel>> hops_;
    // The channel that reaches the target, opened on the last hop.
    void* target_channel_{nullptr};
};

}  // namespace openscrt::protocols::ssh

#endif  // OPENCRT_PROTOCOLS_SSH_SSH_JUMP_HOST_PROXY_H_
