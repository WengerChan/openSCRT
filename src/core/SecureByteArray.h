// Locked, non-paging buffer for credential plaintext.
//
// Phase 2 security acceptance criterion P2-A3 requires that no plaintext password survives
// in process memory after use. Two mechanisms back that up:
//   * libsodium's guarded allocator (`sodium_malloc_locked`) fills fresh pages with 0xDB so
//     accidental reads fail fast instead of leaking heap data;
//   * `sodium_mlock` keeps the pages out of swap and out of core dumps.
//
// `sodium_mlock` can legitimately fail under a low RLIMIT_MEMLOCK (common in CI containers,
// see docs/DEVELOPMENT_PLAN.md 5.4). We degrade to an unlocked but still zeroed allocation and
// report it through `locked()` instead of crashing.

#ifndef OPENCRT_CORE_SECURE_BYTE_ARRAY_H_
#define OPENCRT_CORE_SECURE_BYTE_ARRAY_H_

#include <cstddef>
#include <cstdint>

namespace openscrt::core {

class SecureByteArray {
 public:
    SecureByteArray() = default;
    explicit SecureByteArray(std::size_t size);

    SecureByteArray(const SecureByteArray&) = delete;
    SecureByteArray& operator=(const SecureByteArray&) = delete;
    SecureByteArray(SecureByteArray&& other) noexcept;
    SecureByteArray& operator=(SecureByteArray&& other) noexcept;

    ~SecureByteArray();

    void resize(std::size_t size);

    // Explicit copy-in; there is no implicit conversion so plaintext cannot leak into a
    // std::string / QByteArray by accident.
    void assign(const void* data, std::size_t size);
    void assign(const char* text);

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool locked() const noexcept { return locked_; }

    [[nodiscard]] std::uint8_t* data() noexcept { return data_; }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }

    // Caller-provided output buffer; keeps the plaintext inside a stack buffer the caller
    // owns and can wipe.
    [[nodiscard]] std::size_t copyOut(void* dst, std::size_t dst_size) const;

    // Best-effort wipe that is not optimised away.
    void wipe() noexcept;

 private:
    void release() noexcept;

    std::uint8_t* data_{nullptr};
    std::size_t size_{0};
    bool locked_{false};
};

}  // namespace openscrt::core

#endif  // OPENCRT_CORE_SECURE_BYTE_ARRAY_H_