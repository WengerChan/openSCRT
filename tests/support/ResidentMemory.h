// Resident set size, for the memory-growth half of the acceptance criteria.
//
// "Memory growth stays under 256 MB" is not a number anyone can read off a heap counter:
// the interesting part is what the process holds, not what it allocated. /proc/self/statm
// answers that on Linux without a platform SDK, and the perf tier -- the tier where the
// criterion is judged -- runs on Linux in CI.
//
// The other platforms return 0, which callers read as "not measurable here" rather than
// "zero growth". Pretending a platform measures nothing would be worse than admitting it
// does not.

#ifndef OPENCRT_TESTS_SUPPORT_RESIDENT_MEMORY_H_
#define OPENCRT_TESTS_SUPPORT_RESIDENT_MEMORY_H_

#include <cstddef>

namespace openscrt::test {

// The process's resident set size in bytes, or 0 when the platform does not expose it.
[[nodiscard]] std::size_t residentBytes();

}  // namespace openscrt::test

#endif  // OPENCRT_TESTS_SUPPORT_RESIDENT_MEMORY_H_
