// One lock for every libssh session in the process.
//
// libssh is not thread-safe. A session has one socket, and whoever reads it
// takes the packets: two threads reading the same session split the packets
// between them, and the stream falls out of step -- the symptom is
// "read_packet(): Packet len too high" and a channel that never opens.
//
// The lock is held while a call that may read or write the socket runs -- and
// only around such calls.
//
// The calls that read the socket themselves hold it, so that two threads do
// not take the packets of the same session between them: libssh's blocking
// paths wait for an answer with ssh_handle_packets, and the channel opening
// and the forwarding request are among them.
//
// The calls that do NOT read the socket themselves must NOT hold it: libssh's
// blocking SFTP calls wait for an answer that the reader thread has to bring,
// and holding the lock there is a deadlock -- that is what the first attempt
// at this lock did, and the SFTP tests timed out.
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
