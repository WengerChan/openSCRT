// The memory scan (P2-A3).
//
// The criterion is that no plaintext password stays in the memory of the process once it
// has been used. This test looks for a marker in the memory of the process itself.
//
// Three things make the test mean something:
//
//   * the marker is random, so it cannot be a word of the source and cannot be in the
//     binary -- if the scan finds it, something put it there at run time;
//   * the scan has to find the marker while the buffer that holds it is alive. Without
//     that, the test could pass without having looked at anything;
//   * the region that holds the needle is not read. The needle is the thing being looked
//     for, and a copy of it in the memory would be found by every scan, no matter what
//     the program under test did.
//
// The three platforms are read three different ways, because that is what each of them
// offers: /proc/self/mem on Linux, mach_vm_read on macOS, ReadProcessMemory on Windows.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include "core/SecureByteArray.h"

namespace openscrt::core {
namespace {

// What a look at the memory of this process found.
enum class ScanResult {
    kFound,
    kNotFound,
    kCouldNotLook,
};

// A readable region of the memory of this process.
struct Region {
    std::uintptr_t address{0};
    std::size_t size{0};
};

#if defined(__linux__)

// The regions are the ones /proc/self/maps lists, and the bytes are read out of
// /proc/self/mem.
std::vector<Region> readableRegions() {
    std::vector<Region> regions;
    std::ifstream maps("/proc/self/maps");
    if (!maps) {
        return regions;
    }
    std::string line;
    while (std::getline(maps, line)) {
        // "7f...-7f... r--p 00000000 00:00 0 [heap]"
        const std::size_t dash = line.find('-');
        const std::size_t space = line.find(' ');
        if (dash == std::string::npos || space == std::string::npos || dash > space) {
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
        regions.push_back(Region{begin, static_cast<std::size_t>(end - begin)});
    }
    return regions;
}

std::size_t readMemoryAt(std::uintptr_t address, std::size_t size, std::uint8_t* out) {
    const int fd = ::open("/proc/self/mem", O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    const ssize_t got = ::pread(fd, out, size, static_cast<off_t>(address));
    ::close(fd);
    return got > 0 ? static_cast<std::size_t>(got) : 0;
}

#elif defined(__APPLE__)

// The regions come from mach_vm_region, the bytes from mach_vm_read.
//
// The two declarations are as the modern SDK has them: mach_vm_read takes the buffer
// before the count, and the buffer is a vm_offset_t pointer -- an integer type used as
// a pointer, which is what MIG generates. mach_vm_region takes the region info as a
// pointer to the union and six arguments; the older 10.6 SDK had a seventh argument and
// a different type for the info, which is what the first version of this code used --
// and the macOS leg of CI said so, twice, in two different ways.
std::vector<Region> readableRegions() {
    std::vector<Region> regions;
    vm_address_t address = 0;
    for (;;) {
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        const kern_return_t status =
            mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                           reinterpret_cast<vm_region_info_data_t*>(&info), &count);
        if (status != KERN_SUCCESS) {
            break;
        }
        if (size == 0) {
            break;
        }
        if ((info.protection & VM_PROT_READ) != 0) {
            regions.push_back(
                Region{static_cast<std::uintptr_t>(address), static_cast<std::size_t>(size)});
        }
        address += size;
    }
    return regions;
}

std::size_t readMemoryAt(std::uintptr_t address, std::size_t size, std::uint8_t* out) {
    mach_msg_type_number_t count = static_cast<mach_msg_type_number_t>(size);
    const kern_return_t status = mach_vm_read(mach_task_self(), static_cast<vm_address_t>(address),
                                              size, reinterpret_cast<vm_offset_t*>(out), &count);
    if (status != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<std::size_t>(count);
}

#elif defined(_WIN32)

// The regions come from VirtualQuery, the bytes from ReadProcessMemory on our own
// process.
std::vector<Region> readableRegions() {
    std::vector<Region> regions;
    MEMORY_BASIC_INFORMATION info;
    std::uintptr_t address = 0;
    while (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == sizeof(info)) {
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0 &&
            (info.Protect & PAGE_NOACCESS) == 0) {
            regions.push_back(Region{base, static_cast<std::size_t>(info.RegionSize)});
        }
        const std::uintptr_t next = base + static_cast<std::uintptr_t>(info.RegionSize);
        if (next <= address) {
            break;
        }
        address = next;
    }
    return regions;
}

std::size_t readMemoryAt(std::uintptr_t address, std::size_t size, std::uint8_t* out) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size,
                           &got)) {
        return 0;
    }
    return static_cast<std::size_t>(got);
}

#else

std::vector<Region> readableRegions() {
    return {};
}

std::size_t readMemoryAt(std::uintptr_t, std::size_t, std::uint8_t*) {
    return 0;
}

#endif

// Looks for the needle in the readable memory of this process.
//
// The memory is read in blocks, and the blocks overlap by the length of the needle, so a
// marker that sits on the border of two blocks is still found. The block is wiped after
// every search: a marker that was found in it must not be found again in the next block.
ScanResult scanProcessMemoryFor(const std::uint8_t* needle, std::size_t needle_size) {
    if (needle == nullptr || needle_size == 0) {
        return ScanResult::kCouldNotLook;
    }
    const std::vector<Region> regions = readableRegions();
    if (regions.empty()) {
        return ScanResult::kCouldNotLook;
    }
    const std::uintptr_t needle_address = reinterpret_cast<std::uintptr_t>(needle);

    constexpr std::size_t kBlock = 1u << 20;
    std::vector<std::uint8_t> block(kBlock + needle_size);
    std::vector<std::uint8_t> overlap(needle_size);

    for (const Region& region : regions) {
        // The region that holds the needle is the needle's own copy. Reading it would
        // find the needle whatever the program under test did.
        if (needle_address >= region.address && needle_address < region.address + region.size) {
            continue;
        }
        auto address = region.address;
        while (address < region.address + region.size) {
            const std::size_t wanted =
                std::min(kBlock, static_cast<std::size_t>(region.address + region.size - address));
            const std::size_t got = readMemoryAt(address, wanted, block.data() + needle_size);
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
            std::copy(block.begin() + got, block.begin() + needle_size + got, overlap.begin());
            address += got;
        }
    }
    std::fill(block.begin(), block.end(), 0);
    std::fill(overlap.begin(), overlap.end(), 0);
    return ScanResult::kNotFound;
}

class SecureByteArrayMemoryTest : public ::testing::Test {};

TEST_F(SecureByteArrayMemoryTest, APlaintextThatWasUsedIsNotInTheMemoryAfterwards) {
    // The marker is random: a word of the source would be in the binary too, and the
    // scan would find it there whether the buffer had it or not.
    std::array<std::uint8_t, 32> marker{};
    {
        std::random_device device;
        for (auto& byte : marker) {
            byte = static_cast<std::uint8_t>(device());
        }
    }
    // A block of zeros is everywhere in the memory.
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
}

TEST_F(SecureByteArrayMemoryTest, TheScanCanLookAtTheMemoryAtAll) {
    // A scan that cannot read anything would report "not found" for everything, and the
    // test above would pass for the wrong reason.
    std::array<std::uint8_t, 32> marker{};
    {
        std::random_device device;
        for (auto& byte : marker) {
            byte = static_cast<std::uint8_t>(device());
        }
    }
    if (std::all_of(marker.begin(), marker.end(), [](std::uint8_t byte) { return byte == 0; })) {
        GTEST_SKIP() << "the random source gave nothing to look for";
    }

    SecureByteArray buffer;
    buffer.assign(marker.data(), marker.size());
    EXPECT_EQ(scanProcessMemoryFor(marker.data(), marker.size()), ScanResult::kFound)
        << "the scan cannot read the memory of this process";
}

}  // namespace
}  // namespace openscrt::core
