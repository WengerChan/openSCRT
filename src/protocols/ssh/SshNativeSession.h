// A handle to the libssh session behind a channel.
//
// The point of this file is that libssh's types stay behind it. The transfer layer
// needs the session -- to open an SFTP subsystem on it, or a forwarded channel -- and
// the protocols layer has it, but neither of them should have to include libssh's
// headers to say so.
//
// The handle also carries the reference that keeps the session alive. Without it, a
// subsystem opened on a session that is disconnected a moment later would be reading
// from freed memory.

#ifndef OPENCRT_PROTOCOLS_SSH_SSH_NATIVE_SESSION_H_
#define OPENCRT_PROTOCOLS_SSH_SSH_NATIVE_SESSION_H_

#include <memory>
#include <utility>

namespace openscrt::protocols::ssh {

class SshNativeSession {
 public:
    SshNativeSession() = default;

    // `handle` is the libssh session; `keepalive` is whatever owns it. The type of
    // the handle is libssh's business and stays behind this header.
    SshNativeSession(void* handle, std::shared_ptr<void> keepalive)
        : handle_(handle), keepalive_(std::move(keepalive)) {}

    [[nodiscard]] void* get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

 private:
    void* handle_{nullptr};
    std::shared_ptr<void> keepalive_;
};

}  // namespace openscrt::protocols::ssh

#endif  // OPENCRT_PROTOCOLS_SSH_SSH_NATIVE_SESSION_H_
