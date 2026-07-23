#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace backtrack {

template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(size_t capacity)
        : buffer_(std::max<size_t>(capacity, 1) + 1) {
    }

    // Safe only when producer and consumer are stopped (no concurrent push/pop).
    void resetCapacity(size_t capacity) {
        clear();
        buffer_.assign(std::max<size_t>(capacity, 1) + 1, T{});
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    bool tryPush(T&& item) {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = increment(head);
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool tryPop(T& item) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        item = std::move(buffer_[tail]);
        tail_.store(increment(tail), std::memory_order_release);
        return true;
    }

    size_t size() const {
        const auto head = head_.load(std::memory_order_acquire);
        const auto tail = tail_.load(std::memory_order_acquire);
        if (head >= tail) {
            return head - tail;
        }
        return buffer_.size() - tail + head;
    }

    size_t capacity() const {
        return buffer_.empty() ? 0 : buffer_.size() - 1;
    }

    void clear() {
        T item{};
        while (tryPop(item)) {
        }
    }

private:
    size_t increment(size_t value) const {
        return (value + 1) % buffer_.size();
    }

    std::vector<T> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace backtrack
