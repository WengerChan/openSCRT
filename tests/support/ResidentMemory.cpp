#include "support/ResidentMemory.h"

#include <fstream>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace openscrt::test {

std::size_t residentBytes() {
#if defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    if (!statm) {
        return 0;
    }
    std::size_t total_pages = 0;
    std::size_t resident_pages = 0;
    if (!(statm >> total_pages >> resident_pages)) {
        return 0;
    }
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return 0;
    }
    return resident_pages * static_cast<std::size_t>(page_size);
#else
    return 0;
#endif
}

}  // namespace openscrt::test
