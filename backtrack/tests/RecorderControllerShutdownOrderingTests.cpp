#include "app/AudioDrain.h"
#include "core/SpscQueue.h"
#include "core/Types.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Regression: a producer holding a packet when stopPipeline runs must not lose
// that packet. The consumer (drainAudioQueues) must not exit on the shared stop
// flag alone — it must wait until the producers-finished flag is set (after the
// producer threads have been joined). This test:
//   1. Starts the consumer with producersFinished=false.
//   2. A producer pauses just before enqueueing a "pre-stop" packet.
//   3. The main thread simulates the stop (producersFinished stays false).
//      With the old code the consumer would exit here (empty queues + stop flag).
//   4. The producer is released and enqueues the packet.
//   5. producersFinished is set, the consumer drains the packet and exits.
//   6. Assert the packet was consumed.
void testLatePacketConsumed() {
    backtrack::SpscQueue<backtrack::AudioPacket> systemQueue(16);
    backtrack::SpscQueue<backtrack::AudioPacket> microphoneQueue(16);
    std::atomic<bool> producersFinished{false};
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    expect(event != nullptr, "CreateEvent failed");

    std::vector<int> consumed;
    std::mutex consumedMutex;

    // Consumer thread running the production drain loop.
    std::thread consumer([&] {
        drainAudioQueues(systemQueue, microphoneQueue, producersFinished, event,
            [&](backtrack::AudioPacket&& packet) {
                std::scoped_lock lock(consumedMutex);
                consumed.push_back(packet.frameCount);
            });
    });

    // Producer: signal readiness, pause before enqueue, then push.
    std::atomic<bool> producerReady{false};
    std::atomic<bool> producerGo{false};
    std::thread producer([&] {
        backtrack::AudioPacket packet;
        packet.track = backtrack::AudioTrack::System;
        packet.frameCount = 42;
        producerReady.store(true, std::memory_order_release);
        while (!producerGo.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        systemQueue.tryPush(std::move(packet));
        SetEvent(event);
    });

    // Wait for the producer to be ready (paused before enqueue).
    while (!producerReady.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Give the consumer a chance to observe empty queues.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // At this point the consumer is alive (producersFinished is false).
    // With the old code (stopRequested_ check), the consumer would have exited.
    // With the fix (producersFinished check), the consumer keeps waiting.

    // Release the producer to enqueue its packet.
    producerGo.store(true, std::memory_order_release);
    producer.join();

    // Signal that all producers have finished.
    producersFinished.store(true, std::memory_order_release);
    SetEvent(event);

    consumer.join();
    CloseHandle(event);

    // The late packet must have been consumed before the writer exited.
    expect(consumed.size() == 1 && consumed[0] == 42,
           "late packet was not consumed before writer exit");
}

// Same scenario but with the microphone queue instead of system queue.
void testLatePacketConsumedMicrophone() {
    backtrack::SpscQueue<backtrack::AudioPacket> systemQueue(16);
    backtrack::SpscQueue<backtrack::AudioPacket> microphoneQueue(16);
    std::atomic<bool> producersFinished{false};
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    expect(event != nullptr, "CreateEvent failed");

    std::vector<int> consumed;
    std::mutex consumedMutex;

    std::thread consumer([&] {
        drainAudioQueues(systemQueue, microphoneQueue, producersFinished, event,
            [&](backtrack::AudioPacket&& packet) {
                std::scoped_lock lock(consumedMutex);
                consumed.push_back(packet.frameCount);
            });
    });

    std::atomic<bool> producerReady{false};
    std::atomic<bool> producerGo{false};
    std::thread producer([&] {
        backtrack::AudioPacket packet;
        packet.track = backtrack::AudioTrack::Microphone;
        packet.frameCount = 99;
        producerReady.store(true, std::memory_order_release);
        while (!producerGo.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        microphoneQueue.tryPush(std::move(packet));
        SetEvent(event);
    });

    while (!producerReady.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    producerGo.store(true, std::memory_order_release);
    producer.join();

    producersFinished.store(true, std::memory_order_release);
    SetEvent(event);

    consumer.join();
    CloseHandle(event);

    expect(consumed.size() == 1 && consumed[0] == 99,
           "late microphone packet was not consumed before writer exit");
}

// Consumer must drain packets that were enqueued before producersFinished,
// even when the queues are non-empty at the time producersFinished is set.
void testConsumerDrainsBeforeExit() {
    backtrack::SpscQueue<backtrack::AudioPacket> systemQueue(16);
    backtrack::SpscQueue<backtrack::AudioPacket> microphoneQueue(16);
    std::atomic<bool> producersFinished{false};
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    expect(event != nullptr, "CreateEvent failed");

    std::vector<int> consumed;
    std::mutex consumedMutex;

    // Pre-load some packets before the consumer starts.
    {
        backtrack::AudioPacket p1;
        p1.track = backtrack::AudioTrack::System;
        p1.frameCount = 1;
        systemQueue.tryPush(std::move(p1));
        backtrack::AudioPacket p2;
        p2.track = backtrack::AudioTrack::Microphone;
        p2.frameCount = 2;
        microphoneQueue.tryPush(std::move(p2));
    }

    std::thread consumer([&] {
        drainAudioQueues(systemQueue, microphoneQueue, producersFinished, event,
            [&](backtrack::AudioPacket&& packet) {
                std::scoped_lock lock(consumedMutex);
                consumed.push_back(packet.frameCount);
            });
    });

    // Let consumer start draining.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Signal producers finished — consumer must drain remaining packets.
    producersFinished.store(true, std::memory_order_release);
    SetEvent(event);

    consumer.join();
    CloseHandle(event);

    expect(consumed.size() == 2, "consumer did not drain all pre-loaded packets");
}

} // namespace

int main() {
    try {
        testLatePacketConsumed();
        testLatePacketConsumedMicrophone();
        testConsumerDrainsBeforeExit();
    } catch (const std::exception& error) {
        std::cerr << "RecorderControllerShutdownOrderingTests: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
