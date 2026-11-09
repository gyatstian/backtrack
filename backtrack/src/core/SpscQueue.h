#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace backtrack {

template <typename T>
class SpscQueue {
    // clear() drains through tryPop, which assigns into a default-constructed
    // T. Both requirements are documented here rather than discovered at the
    // first instantiation that breaks.
    static_assert(std::is_default_constructible_v<T>,
        "SpscQueue::clear() drains into a default-constructed T");
    static_assert(std::is_move_assignable_v<T> || std::is_copy_assignable_v<T>,
        "SpscQueue hands items out by assigning into the caller's T");

public:
    // The ring storage is fixed for the queue's lifetime at this capacity;
    // resetCapacity only ever lowers an atomic logical limit, so metadata
    // observers (size()/capacity()) never race a backing-store reassignment.
    explicit SpscQueue(size_t capacity)
        : buffer_(std::max<size_t>(capacity, 1) + 1),
          capacity_(std::max<size_t>(capacity, 1)) {
    }

    // Changes only the logical limit (clamped to the fixed storage) and never
    // touches the backing vector, so size()/capacity() stay safe to call from
    // any thread, even while this runs (the UI diagnostics timer reads the
    // queue depth during a pipeline restart). Still legal only when producer
    // and consumer are stopped (no concurrent push/pop).
    void resetCapacity(size_t capacity) {
        clear();
        capacity_.store(std::min(std::max<size_t>(capacity, 1), buffer_.size() - 1),
                        std::memory_order_relaxed);
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    bool tryPush(T&& item) {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);
        // Enforce the logical limit against the fixed ring. The producer only
        // advances head_, so a stale tail can only overestimate the count and
        // never admits more than capacity() items.
        const auto count = head >= tail ? head - tail : buffer_.size() - tail + head;
        if (count >= capacity_.load(std::memory_order_relaxed)) {
            return false;
        }

        buffer_[head] = std::move(item);
        head_.store(increment(head), std::memory_order_release);
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

    // Advisory depth. head_ and tail_ are read independently, so a consumer
    // pop landing between the two loads makes the wrapped difference alias to
    // a near-capacity or huge value. The clamp keeps callers (GPU-protection
    // drop decisions, UI queue depth) from ever acting on a bogus size; the
    // result remains approximate by design.
    size_t size() const {
        const auto head = head_.load(std::memory_order_acquire);
        const auto tail = tail_.load(std::memory_order_acquire);
        if (head >= tail) {
            return head - tail;
        }
        return std::min(buffer_.size() - tail + head, capacity());
    }

    size_t capacity() const {
        return capacity_.load(std::memory_order_relaxed);
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
    // Logical item limit (<= buffer_.size() - 1). Only resetCapacity writes it
    // (with both ends stopped); size()/capacity() read it lock-free.
    alignas(64) std::atomic<size_t> capacity_{0};
};

} // namespace backtrack
