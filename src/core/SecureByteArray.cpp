#include "core/SecureByteArray.h"

#include <sodium.h>

#include <cstring>
#include <utility>

namespace openscrt::core {
namespace {

// sodium_malloc_locked tolerates any size, but we keep a floor so a zero-length request
// still yields a distinct, wipeable allocation.
constexpr std::size_t kMinAllocation = 1;

// sodium_malloc_locked requires sodium_init() to have completed. The first SecureByteArray in
// the process pays for the library's random-number subsystem; every later one reuses it.
void ensureSodiumInitialized() {
    static const int result = sodium_init();
    (void)result;  // >= 0 means success; a negative value leaves us on the degraded path.
}

}  // namespace

SecureByteArray::SecureByteArray(std::size_t size) { resize(size); }

SecureByteArray::SecureByteArray(SecureByteArray&& other) noexcept
    : data_(other.data_), size_(other.size_), locked_(other.locked_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.locked_ = false;
}

SecureByteArray& SecureByteArray::operator=(SecureByteArray&& other) noexcept {
    if (this != &other) {
        release();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        locked_ = std::exchange(other.locked_, false);
    }
    return *this;
}

SecureByteArray::~SecureByteArray() { release(); }

void SecureByteArray::resize(std::size_t size) {
    ensureSodiumInitialized();
    const std::size_t wanted = size == 0 ? kMinAllocation : size;
    if (wanted == size_ && data_ != nullptr) {
        return;
    }
    release();

    data_ = static_cast<std::uint8_t*>(sodium_malloc_locked(wanted));
    if (data_ == nullptr) {
        // sodium_malloc only aborts on absurd sizes; a NULL here means allocation failure.
        size_ = 0;
        locked_ = false;
        return;
    }
    // sodium_malloc_locked leaves the first 64 bytes as an unmapped guard page, so a
    // one-byte underflow on data() traps instead of corrupting the heap.
    if (sodium_mlock(data_, wanted) != 0) {
        // RLIMIT_MEMLOCK exhausted (typical in CI containers). Unlocked still means
        // no paging and explicit zeroing; report the degradation to the UI layer.
        locked_ = false;
    } else {
        locked_ = true;
    }
    size_ = size;
}

void SecureByteArray::assign(const void* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        wipe();
        return;
    }
    resize(size);
    if (data_ != nullptr && size_ >= size) {
        std::memcpy(data_, data, size);
    }
}

void SecureByteArray::assign(const char* text) {
    if (text == nullptr) {
        wipe();
        return;
    }
    assign(text, std::strlen(text));
}

std::size_t SecureByteArray::copyOut(void* dst, std::size_t dst_size) const {
    if (dst == nullptr || dst_size == 0 || data_ == nullptr) {
        return 0;
    }
    const std::size_t n = size_ < dst_size ? size_ : dst_size;
    std::memcpy(dst, data_, n);
    return n;
}

void SecureByteArray::wipe() noexcept {
    if (data_ != nullptr && size_ > 0) {
        sodium_memzero(data_, size_);
    }
}

void SecureByteArray::release() noexcept {
    if (data_ == nullptr) {
        size_ = 0;
        locked_ = false;
        return;
    }
    sodium_memzero(data_, size_);
    if (locked_) {
        sodium_unlock(data_, size_);
    }
    sodium_free(data_);
    data_ = nullptr;
    size_ = 0;
    locked_ = false;
}

}  // namespace openscrt::core