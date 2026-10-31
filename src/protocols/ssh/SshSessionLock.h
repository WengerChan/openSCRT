// One lock per libssh session.
//
// libssh is not thread-safe. A session has one socket, and whoever reads it takes
// the packets: two threads reading the same session split the packets between
// them, and the stream falls out of step -- the symptom is
// "read_packet(): Packet len too high" and a channel that never opens.
//
// The lock is per session, and not one for the whole process. A call that waits
// for an answer holds the lock for as long as the answer takes, and one
// session's answer must not hold up every other session's reader: with one lock
// for the whole process, twenty sessions' readers take turns, and the one that
// is waiting for an answer keeps the others out.
//
// The lock is held while a call that may read or write the socket runs -- and
// only around such calls. It is held across a poll and the read that follows
// it, because the poll says "there is data" and the read takes it: if the lock
// were let go in between, another thread could take the data, and the read
// would then wait for data that is not coming -- while holding the lock, which
// is how one session's reader ends up stopping every other thread.
//
// The lock is NOT held while a call waits for something another thread has to
// bring. libssh's own blocking reads read the socket themselves (the SFTP
// subsystem's calls go through ssh_channel_read, which reads the socket), so a
// call that waits for an answer makes progress by itself.

#ifndef OPENCRT_PROTOCOLS_SSH_SSH_SESSION_LOCK_H_
#define OPENCRT_PROTOCOLS_SSH_SSH_SESSION_LOCK_H_

#include <map>
#include <mutex>

namespace openscrt::protocols::ssh {
namespace detail {

// The locks, one per session.
//
// The map is never cleared. A session that is gone leaves its lock behind, and
// the address of a session that was freed can be given to the next one -- a lock
// that is used by two sessions in turn is still a lock. The map is deliberately
// leaked so that a lock outlives every static that could be destroyed before it.
//
// It lives in the header because two libraries need it -- the protocols layer and
// the transfer layer -- and a lock that exists in two copies is two locks, which
// is the thing this file exists to prevent. An inline function has one copy in
// the program, and one static with it.
inline std::map<void*, std::mutex>& sessionLocks() {
    static auto* locks = new std::map<void*, std::mutex>();
    return *locks;
}

// The lock of the map itself. It is held for as long as it takes to look a lock
// up, which is nothing compared to the work the lock itself guards.
inline std::mutex& sessionLocksMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace detail

// The lock of one libssh session. The handle is the session's own pointer, so that
// two parts of the same session find the same lock.
inline std::mutex& sshSessionLock(void* session) {
    const std::lock_guard<std::mutex> guard(detail::sessionLocksMutex());
    return detail::sessionLocks()[session];
}

}  // namespace openscrt::protocols::ssh

#endif  // OPENCRT_PROTOCOLS_SSH_SSH_SESSION_LOCK_H_
