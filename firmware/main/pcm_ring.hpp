#pragma once

#include <cstddef>
#include <cstdint>

namespace jarvis {
// Storage and synchronization belong to the caller. Full buffers reject new samples.
class PcmRing {
public:
    void bind(std::int16_t* storage, std::size_t capacity) {
        data_ = storage;
        capacity_ = capacity;
        clear();
    }
    void clear() { read_ = write_ = count_ = 0; }
    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    bool push(std::int16_t sample) {
        if (count_ == capacity_) return false;
        data_[write_] = sample;
        write_ = (write_ + 1) % capacity_;
        ++count_;
        return true;
    }
    std::size_t pop(std::int16_t* dest, std::size_t max) {
        const std::size_t n = count_ < max ? count_ : max;
        for (std::size_t i = 0; i < n; ++i) {
            dest[i] = data_[read_];
            read_ = (read_ + 1) % capacity_;
        }
        count_ -= n;
        return n;
    }
private:
    std::int16_t* data_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t read_ = 0, write_ = 0, count_ = 0;
};
} // namespace jarvis
