#include "protocols/ssh/SshJumpHostProxy.h"

#include <libssh/libssh.h>

#include <utility>

namespace openscrt::protocols::ssh {

SshJumpHostProxy::SshJumpHostProxy(JumpHostChain chain) : chain_(std::move(chain)) {}

SshJumpHostProxy::~SshJumpHostProxy() {
    if (target_channel_ != nullptr) {
        ssh_channel_free(static_cast<ssh_channel>(target_channel_));
        target_channel_ = nullptr;
    }
    hops_.clear();
}

TransportResult SshJumpHostProxy::connect() {
    if (chain_.target_host.empty()) {
        return TransportResult{false, "the chain has no target"};
    }

    // Every hop is connected in turn. The last one is the session the target is
    // reached through.
    for (std::size_t index = 0; index < chain_.hops.size(); ++index) {
        JumpHostStep& step = chain_.hops[index];
        SshConnectionOptions options;
        options.host = step.host;
        options.port = step.port;
        options.user = step.user;
        options.private_key_path = step.private_key_path;
        // The password is a locked buffer, which cannot be copied. The chain is
        // consumed by the attempt, so it is moved rather than copied.
        options.password = std::move(step.password);

        auto hop = std::make_unique<Ssh2Channel>(std::move(options));
        const TransportResult result = hop->connectAsync();
        if (!result.ok) {
            const std::string message = "the hop " + std::to_string(index + 1) + " (" + step.host +
                                        ") could not be reached: " + result.error;
            hops_.clear();
            return TransportResult{false, message};
        }
        hops_.push_back(std::move(hop));
    }

    if (hops_.empty()) {
        // No hop: the target has to be reached directly, which this proxy does not
        // do. It would have to be a plain SSH channel.
        return TransportResult{false, "a chain without a hop has nothing to go through"};
    }

    // The channel that reaches the target is opened on the last hop.
    SshNativeSession session = hops_.back()->nativeSession();
    auto* native = static_cast<ssh_session>(session.get());
    ssh_channel channel = ssh_channel_new(native);
    if (channel == nullptr) {
        hops_.clear();
        return TransportResult{false, "the channel to the target could not be opened"};
    }
    if (ssh_channel_open_forward(channel, chain_.target_host.c_str(), chain_.target_port,
                                 "127.0.0.1", 0) != SSH_OK) {
        const char* reason = ssh_get_error(native);
        ssh_channel_free(channel);
        hops_.clear();
        return TransportResult{false,
                               std::string("the target could not be reached: ") +
                                   (reason != nullptr && *reason != '\0' ? reason : "no reason")};
    }
    target_channel_ = channel;
    return TransportResult{true, {}};
}

SshNativeSession SshJumpHostProxy::lastHopSession() const noexcept {
    if (hops_.empty()) {
        return SshNativeSession();
    }
    return hops_.back()->nativeSession();
}

}  // namespace openscrt::protocols::ssh
