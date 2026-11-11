#pragma once

#include <algorithm>
#include <vector>

namespace fixomic {

// Fixed-capacity ring buffer. Not thread-safe: producer and consumer must be the same thread
// (the audio callback). All allocation happens in the constructor; the other members never
// allocate, lock or log, so they are safe to call from the real-time thread.
template <typename T>
class FrameFifo {
public:
    explicit FrameFifo(int capacity) : buffer_(static_cast<size_t>(std::max(capacity, 1))) {}

    int capacity() const noexcept { return static_cast<int>(buffer_.size()); }
    int size() const noexcept { return count_; }

    // Returns false (and writes nothing) if fewer than n free slots remain.
    bool push(const T* src, int n) noexcept {
        if (n < 0 || n > capacity() - count_) return false;
        int pos = writePos();
        for (int i = 0; i < n; ++i) {
            buffer_[static_cast<size_t>(pos)] = src[i];
            if (++pos == capacity()) pos = 0;
        }
        count_ += n;
        return true;
    }

    // Returns false (and reads nothing) if fewer than n samples are available.
    bool pop(T* dst, int n) noexcept {
        if (n < 0 || n > count_) return false;
        for (int i = 0; i < n; ++i) {
            dst[i] = buffer_[static_cast<size_t>(head_)];
            if (++head_ == capacity()) head_ = 0;
        }
        count_ -= n;
        return true;
    }

    // Pushes up to n zeros; silently limited to the free space.
    void pushZeros(int n) noexcept {
        n = std::min(n, capacity() - count_);
        int pos = writePos();
        for (int i = 0; i < n; ++i) {
            buffer_[static_cast<size_t>(pos)] = T{};
            if (++pos == capacity()) pos = 0;
        }
        count_ += std::max(n, 0);
    }

    void clear() noexcept {
        head_ = 0;
        count_ = 0;
    }

private:
    int writePos() const noexcept {
        const int pos = head_ + count_;
        return pos >= capacity() ? pos - capacity() : pos;
    }

    std::vector<T> buffer_;
    int head_ = 0;
    int count_ = 0;
};

} // namespace fixomic
