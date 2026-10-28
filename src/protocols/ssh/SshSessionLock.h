// One lock for every libssh session in the process.
//
// libssh is not thread-safe. A session has one socket, and whoever reads it
// takes the packets: two threads reading the same session split the packets
// between them, and the stream falls out of step -- the symptom is
// "read_packet(): Packet len too high" and a channel that never opens.
//
// The lock is held while a call that may read or write the socket runs. It is
// never held while a thread waits for something that only another thread can
// bring: libssh's blocking calls (the SFTP requests, the opening of a
// channel, the forwarding request) read the socket themselves, so the thread
// that holds the lock is the thread that makes progress.
//
// One lock for the whole process, and not one per session, because the pieces
// that share a session -- the channel, the SFTP subsystem, the port forwarder,
// the jump host -- only know the session by its handle, and a handle is not
// something a map of locks should be keyed on.

#ifndef OPENCRT_PROTOCOLS_SSH_SSH_SESSION_LOCK_H_
#define OPENCRT_PROTOCOLS_SSH_SSH_SESSION_LOCK_H_

#include <mutex>

namespace openscrt::protocols::ssh {

// The lock every libssh call in this process goes through.
std::mutex& sshSessionLock();

}  // namespace openscrt::protocols::ssh

#endif  // OPENCRT_PROTOCOLS_SSH_SSH_SESSION_LOCK_H_
