#include "core/SpscQueue.h"

#include <atomic>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using backtrack::SpscQueue;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Slot {
    int id = 0;
};

// A move that clears its source, so a rejected tryPush can be shown to have left
// the caller's item alone. tryPush takes T&&, so the caller hands over a named
// item and inspects it afterwards.
struct MoveProbe {
    std::wstring payload;
    int* moves = nullptr;

    MoveProbe() = default;
    MoveProbe(std::wstring text, int* moveCounter)
        : payload(std::move(text)), moves(moveCounter) {
    }
    MoveProbe(MoveProbe&& other) noexcept
        : payload(std::move(other.payload)), moves(other.moves) {
        if (moves != nullptr) {
            ++*moves;
        }
        other.payload.clear();
    }
    MoveProbe& operator=(MoveProbe&& other) noexcept {
        payload = std::move(other.payload);
        moves = other.moves;
        if (moves != nullptr) {
            ++*moves;
        }
        other.payload.clear();
        return *this;
    }
};

bool push(SpscQueue<Slot>& queue, int id) {
    return queue.tryPush(Slot{id});
}

void testPushPopKeepsFifoOrder() {
    SpscQueue<Slot> queue(8);
    expect(queue.capacity() == 8, "the queue did not report the capacity it was built with");
    expect(queue.size() == 0, "a fresh queue did not start empty");

    Slot item{};
    expect(!queue.tryPop(item), "a pop from a fresh queue reported an item");
    expect(item.id == 0, "a failed pop overwrote the caller's item");

    for (int id = 1; id <= 8; ++id) {
        expect(push(queue, id), "the queue refused an item below capacity");
    }
    for (int id = 1; id <= 8; ++id) {
        expect(queue.tryPop(item), "the queue lost an item it had accepted");
        expect(item.id == id, "the queue did not return items in submission order");
    }
    expect(!queue.tryPop(item), "the queue handed out one item more than it accepted");
    expect(queue.size() == 0, "the queue is not empty after draining it");
}

void testFullQueueRejectsWithoutMovingTheItem() {
    SpscQueue<MoveProbe> queue(2);
    int accepted = 0;
    for (int id = 0; id < 2; ++id) {
        MoveProbe item(L"accepted", &accepted);
        expect(queue.tryPush(std::move(item)), "the queue refused an item below capacity");
        expect(accepted == id + 1, "an accepted item was not moved into the queue");
    }

    // Full: the overflow policy belongs to the caller, so the only guarantee is
    // that the refusal costs the caller nothing.
    int rejected = 0;
    MoveProbe overflow(L"overflow", &rejected);
    expect(!queue.tryPush(std::move(overflow)), "the queue accepted an item while full");
    expect(rejected == 0, "a rejected tryPush moved the caller's item anyway");
    expect(overflow.payload == L"overflow", "a rejected tryPush emptied the caller's item");
    expect(queue.size() == 2, "a rejected tryPush changed the queue depth");

    // The refusal must not have consumed a slot either: the queue still drains
    // exactly what it accepted.
    MoveProbe item;
    for (int id = 0; id < 2; ++id) {
        expect(queue.tryPop(item), "the queue lost an item it had accepted");
    }
    expect(!queue.tryPop(item), "a rejected tryPush left a slot behind");
}

void testSizeAcrossAWrap() {
    // capacity 4 means a five-slot ring, so head and tail wrap independently.
    SpscQueue<Slot> queue(4);
    std::vector<size_t> observed;

    for (int id = 0; id < 4; ++id) {
        expect(push(queue, id), "filling the ring below capacity failed");
    }
    observed.push_back(queue.size());

    Slot item{};
    for (int id = 0; id < 6; ++id) {
        expect(queue.tryPop(item), "draining the ring failed");
        observed.push_back(queue.size());
        expect(push(queue, 100 + id), "refilling the ring below capacity failed");
        observed.push_back(queue.size());
    }

    // Hand-computed: four pushes fill it, then every pop/push pair holds the
    // depth steady while the ring index wraps twice.
    const std::vector<size_t> expected{4, 3, 4, 3, 4, 3, 4, 3, 4, 3, 4, 3, 4};
    expect(observed == expected, "size() did not track the hand-computed depth across a wrap");
    expect(queue.size() <= queue.capacity(), "size() exceeded the capacity after wrapping");

    // Order survived the wrap as well. Six pops consumed 0..3 and 100, 101, so
    // four of the six pushes made are still queued: 102..105.
    for (int id = 102; id < 106; ++id) {
        expect(queue.tryPop(item), "the ring lost an item across the wrap");
        expect(item.id == id, "the ring returned wrapped items out of order");
    }
    expect(!queue.tryPop(item), "the ring handed out more items than it accepted");
}

void testClearEmptiesWithoutLosingCapacity() {
    SpscQueue<Slot> queue(4);
    for (int id = 0; id < 4; ++id) {
        expect(push(queue, id), "filling the ring below capacity failed");
    }
    queue.clear();
    expect(queue.size() == 0, "clear() left items in the queue");
    expect(queue.capacity() == 4, "clear() changed the capacity");

    Slot item{};
    expect(!queue.tryPop(item), "clear() did not move the consumer index");

    // A cleared ring takes a full set again, which is what every consumer of
    // clear() (a reset, a session restart) relies on.
    for (int id = 0; id < 4; ++id) {
        expect(push(queue, id), "a cleared ring refused an item below capacity");
    }
    expect(queue.size() == 4, "a cleared ring did not refill to its capacity");
}

void testResetCapacity() {
    SpscQueue<Slot> queue(4);
    for (int id = 0; id < 4; ++id) {
        expect(push(queue, id), "filling the ring below capacity failed");
    }
    queue.resetCapacity(2);
    expect(queue.capacity() == 2, "resetCapacity did not apply the new capacity");
    expect(queue.size() == 0, "resetCapacity left items in the queue");

    Slot item{};
    expect(!queue.tryPop(item), "resetCapacity did not reset the consumer index");

    for (int id = 0; id < 2; ++id) {
        expect(push(queue, id), "a resized ring refused an item below capacity");
    }
    expect(!push(queue, 99), "a resized ring accepted more than its new capacity");
    expect(queue.tryPop(item) && item.id == 0, "a resized ring returned the wrong item");

    // A capacity below one still yields a usable one-slot ring rather than a
    // zero-sized vector that increment() would divide by zero on.
    queue.resetCapacity(0);
    expect(queue.capacity() == 1, "a zero capacity did not fall back to one slot");
    expect(push(queue, 7), "the one-slot ring refused its only slot");
    expect(queue.tryPop(item) && item.id == 7, "the one-slot ring returned the wrong item");
    expect(!queue.tryPop(item), "the one-slot ring handed out an extra item");
}

void testLogicalLimitEnforcedOverLargerStorage() {
    // The ring storage is fixed for the queue's lifetime (8 slots here), so a
    // reset to a smaller limit must be enforced by tryPush, not by shrinking
    // the backing vector.
    SpscQueue<Slot> queue(8);
    queue.resetCapacity(2);
    expect(queue.capacity() == 2, "resetCapacity did not apply the logical limit");
    for (int id = 0; id < 2; ++id) {
        expect(push(queue, id), "the queue refused an item below its logical limit");
    }
    expect(!push(queue, 99), "the queue accepted more than its logical limit");
    expect(queue.size() == 2, "size() did not report the logical depth");

    // A request above the fixed storage clamps to it, and the ring still
    // refills to that full capacity.
    queue.resetCapacity(32);
    expect(queue.capacity() == 8, "a request above the fixed storage was not clamped");
    for (int id = 0; id < 8; ++id) {
        expect(push(queue, id), "the full-size ring refused an item below its capacity");
    }
    expect(!push(queue, -1), "the full-size ring accepted more than its capacity");
}

void testLogicalLimitAcrossWrappedIndices() {
    // Logical limit 2 over an 8-slot ring: head and tail wrap inside a larger
    // ring than the limit, so size() must stay truthful across full
    // empty<->full cycles at every ring position.
    SpscQueue<Slot> queue(8);
    queue.resetCapacity(2);
    Slot item{};
    for (int round = 0; round < 64; ++round) {
        for (int id = 0; id < 2; ++id) {
            expect(push(queue, 100 * round + id), "refill below the logical limit failed");
        }
        expect(queue.size() == 2, "size() misreported a full logical queue");
        expect(queue.size() <= queue.capacity(), "size() exceeded the logical capacity");
        expect(!push(queue, -1), "the logical limit was not enforced at the wrap boundary");
        for (int id = 0; id < 2; ++id) {
            expect(queue.tryPop(item) && item.id == 100 * round + id,
                   "wrapped items came back out of order");
        }
        expect(queue.size() == 0, "a drained logical queue reported depth");
    }
}

void testConcurrentObserverReadsDuringReset() {
    // The race the fix removes: the UI diagnostics timer reads size()/
    // capacity() on one thread while the controller worker runs resetCapacity.
    // The storage is fixed, so the observer only reads atomics and an
    // immutable vector size; a race detector would flag the old backing-vector
    // reassignment here. Both ends stay stopped (no push/pop), matching the
    // reset contract.
    SpscQueue<Slot> queue(8);
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::thread resetter([&]() {
        size_t capacity = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            queue.resetCapacity(capacity);
            capacity = capacity >= 8 ? 1 : capacity + 1;
        }
    });
    std::thread observer([&]() {
        for (int i = 0; i < 200000 && !failed.load(std::memory_order_relaxed); ++i) {
            const size_t depth = queue.size();
            const size_t capacity = queue.capacity();
            // depth can exceed the momentary logical limit (stale head/tail),
            // but never the fixed storage; capacity is always a valid limit.
            if (depth > 8 || capacity == 0 || capacity > 8) {
                failed.store(true, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
        stop.store(true, std::memory_order_relaxed);
    });
    resetter.join();
    observer.join();
    expect(!failed.load(std::memory_order_relaxed),
           "size()/capacity() returned an invalid value during a concurrent reset");
    expect(queue.size() == 0, "the queue was not empty after the concurrent resets stopped");
    const size_t capacity = queue.capacity();
    expect(capacity >= 1 && capacity <= 8,
           "the queue capacity was invalid after the concurrent resets stopped");
}

} // namespace

int main() {
    try {
        testPushPopKeepsFifoOrder();
        testFullQueueRejectsWithoutMovingTheItem();
        testSizeAcrossAWrap();
        testClearEmptiesWithoutLosingCapacity();
        testResetCapacity();
        testLogicalLimitEnforcedOverLargerStorage();
        testLogicalLimitAcrossWrappedIndices();
        testConcurrentObserverReadsDuringReset();
        std::cout << "SpscQueue tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SpscQueue test failure: " << error.what() << '\n';
        return 1;
    }
}
