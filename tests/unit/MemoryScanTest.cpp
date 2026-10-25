// The memory scan (P2-A3).
//
// The criterion is that no plaintext password stays in the memory of the process once it
// has been used. This test looks for a marker in the memory of the process itself:
//
//   * the marker is random, so it cannot be a word of the source and cannot be in the
//     binary -- if the scan finds it, something put it there at run time;
//   * the scan first has to find the marker while the buffer that holds it is alive.
//     Without that, the test could pass without having looked at anything;
//   * after the buffer is wiped, the marker has to be gone.
//
// The needle of the search lives on the stack, and the stack is not read: the needle is
// the very thing being looked for, and a copy of it in the memory would be found by
// every scan, no matter what the program under test did.
//
// Only Linux has this observation for now. The other platforms have no equivalent here
// yet, and the case says so instead of pretending it passed.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "core/SecureByteArray.h"

#if defined(__linux__)
#include <cstdio>
#include <cstdlib>
#endif

namespace openscrt::core {
namespace {

#if defined(__linux__)

// What a look at the memory of this process found.
enum class ScanResult {
    kFound,
    kNotFound,
    kCouldNotLook,
};

// Looks for the needle in the readable memory of this process.
//
// The memory is read in blocks, and the blocks overlap by the length of the needle, so
// a marker that sits on the border of two blocks is still found. The block is wiped
// after every search: a marker that was found in it must not be found again in the
// next block.
ScanResult scanProcessMemoryFor(const std::uint8_t* needle, std::size_t needle_size) {
    if (needle == nullptr || needle_size == 0) {
        return ScanResult::kCouldNotLook;
    }

    std::ifstream maps("/proc/self/maps");
    if (!maps) {
        return ScanResult::kCouldNotLook;
    }
    std::ifstream memory("/proc/self/mem", std::ios::binary);
    if (!memory) {
        return ScanResult::kCouldNotLook;
    }

    constexpr std::size_t kBlock = 1u << 20;
    std::vector<std::uint8_t> block(kBlock + needle_size);
    std::vector<std::uint8_t> overlap(needle_size);

    std::string line;
    while (std::getline(maps, line)) {
        // "7f...-7f... r--p 00000000 00:00 0 [heap]"
        const std::size_t dash = line.find('-');
        const std::size_t space = line.find(' ');
        if (dash == std::string::npos || space == std::string::npos || dash > space) {
            continue;
        }
        // The stack holds the needle of the search, so it is not read.
        if (line.find("[stack]") != std::string::npos) {
            continue;
        }
        const auto begin =
            static_cast<std::uintptr_t>(std::strtoull(line.substr(0, dash).c_str(), nullptr, 16));
        const auto end = static_cast<std::uintptr_t>(
            std::strtoull(line.substr(dash + 1, space - dash - 1).c_str(), nullptr, 16));
        if (end <= begin) {
            continue;
        }
        const std::string permissions = line.substr(space + 1, 4);
        if (permissions.find('r') == std::string::npos) {
            continue;
        }

        auto address = begin;
        while (address < end) {
            const std::size_t wanted = std::min(kBlock, static_cast<std::size_t>(end - address));
            memory.seekg(static_cast<std::streamoff>(address));
            memory.read(reinterpret_cast<char*>(block.data() + needle_size),
                        static_cast<std::streamsize>(wanted));
            const auto got = static_cast<std::size_t>(memory.gcount());
            if (got == 0) {
                break;
            }
            std::copy(overlap.begin(), overlap.end(), block.begin());
            const auto found = std::search(block.begin(), block.begin() + needle_size + got, needle,
                                           needle + needle_size);
            if (found != block.begin() + needle_size + got) {
                std::fill(block.begin(), block.end(), 0);
                std::fill(overlap.begin(), overlap.end(), 0);
                return ScanResult::kFound;
            }
            std::copy(block.begin() + needle_size + got - needle_size,
                      block.begin() + needle_size + got, overlap.begin());
            address += got;
        }
    }
    std::fill(block.begin(), block.end(), 0);
    std::fill(overlap.begin(), overlap.end(), 0);
    return ScanResult::kNotFound;
}

#endif  // __linux__

TEST(SecureByteArrayMemoryTest, APlaintextThatWasUsedIsNotInTheMemoryAfterwards) {
#if !defined(__linux__)
    GTEST_SKIP() << "no way to read the memory of this process on this platform yet";
#else
    // The marker is random: a word of the source would be in the binary too, and the
    // scan would find it there whether the buffer had it or not.
    std::array<std::uint8_t, 32> marker{};
    {
        std::random_device device;
        for (auto& byte : marker) {
            byte = static_cast<std::uint8_t>(device());
        }
    }
    // The marker must not be zero: a block of zeros is everywhere in the memory.
    if (std::all_of(marker.begin(), marker.end(), [](std::uint8_t byte) { return byte == 0; })) {
        GTEST_SKIP() << "the random source gave nothing to look for";
    }

    {
        SecureByteArray buffer;
        buffer.assign(marker.data(), marker.size());

        // The control: the memory of the process really holds the marker while the
        // buffer is alive. Without this the test could pass without looking at anything.
        EXPECT_EQ(scanProcessMemoryFor(marker.data(), marker.size()), ScanResult::kFound)
            << "the scan does not see the buffer, so the rest of the test proves nothing";

        // Wiping it is what the buffer does when it dies.
        buffer.wipe();
        EXPECT_EQ(scanProcessMemoryFor(marker.data(), marker.size()), ScanResult::kNotFound)
            << "the plaintext is still in the memory of the process";
    }

    // And after the buffer is gone as well.
    EXPECT_EQ(scanProcessMemoryFor(marker.data(), marker.size()), ScanResult::kNotFound)
        << "the plaintext is still in the memory of the process";
#endif
}

}  // namespace
}  // namespace openscrt::core
