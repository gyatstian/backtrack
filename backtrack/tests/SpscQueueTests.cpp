#include "core/SpscQueue.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
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

} // namespace

int main() {
    try {
        testPushPopKeepsFifoOrder();
        testFullQueueRejectsWithoutMovingTheItem();
        testSizeAcrossAWrap();
        testClearEmptiesWithoutLosingCapacity();
        testResetCapacity();
        std::cout << "SpscQueue tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SpscQueue test failure: " << error.what() << '\n';
        return 1;
    }
}
