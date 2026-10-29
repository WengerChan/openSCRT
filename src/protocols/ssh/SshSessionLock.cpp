#include "protocols/ssh/SshSessionLock.h"

namespace openscrt::protocols::ssh {

std::mutex& sshSessionLock() {
    // A function-local static: it is built the first time it is asked for, and
    // the build of a static local is thread-safe.
    static std::mutex lock;
    return lock;
}

}  // namespace openscrt::protocols::ssh
