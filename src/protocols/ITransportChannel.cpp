#include "protocols/ITransportChannel.h"

namespace openscrt::protocols {

const char* toString(TransportState state) noexcept {
    switch (state) {
        case TransportState::kIdle:
            return "idle";
        case TransportState::kConnecting:
            return "connecting";
        case TransportState::kConnected:
            return "connected";
        case TransportState::kReconnecting:
            return "reconnecting";
        case TransportState::kDisconnected:
            return "disconnected";
        case TransportState::kFailed:
            return "failed";
    }
    return "unknown";
}

const char* toString(TransportKind kind) noexcept {
    switch (kind) {
        case TransportKind::kSsh2:
            return "ssh2";
        case TransportKind::kSerial:
            return "serial";
        case TransportKind::kTelnet:
            return "telnet";
        case TransportKind::kRlogin:
            return "rlogin";
        case TransportKind::kRawTcp:
            return "raw-tcp";
        case TransportKind::kLocalPty:
            return "local-pty";
    }
    return "unknown";
}

}  // namespace openscrt::protocols