#include "app/AudioDrain.h"
#include "app/AudioWriter.h"
#include "core/SpscQueue.h"
#include "core/Types.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
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

void testBlockRms() {
    const auto format = backtrack::parseAudioFormat(backtrack::canonicalAudioFormat());
    backtrack::AudioPacket packet;
    expect(backtrack::measureBlockLevels(packet, format).rmsDb == -90.0f,
           "empty audio must keep the RMS floor");

    const float samples[] = {0.125f, -0.125f, 0.0f, 0.0f};
    packet.bytes.resize(sizeof(samples));
    std::memcpy(packet.bytes.data(), samples, sizeof(samples));
    const float expectedDb = static_cast<float>(20.0 * std::log10(std::sqrt(0.03125 / 4.0)));
    expect(std::fabs(backtrack::measureBlockLevels(packet, format).rmsDb - expectedDb) < 0.0001f,
           "block level must use RMS across all signed samples, not peak or mean");
    expect(backtrack::measureBlockLevels(packet, {}).rmsDb == -90.0f,
           "a zero sample width must keep the RMS floor");

    packet.bytes.assign(sizeof(samples), 0);
    expect(backtrack::measureBlockLevels(packet, format).rmsDb == -90.0f,
           "digital silence must keep the RMS floor");
}

void testMicrophoneAgcUsesBlockRms() {
    const auto format = backtrack::parseAudioFormat(backtrack::canonicalAudioFormat());
    backtrack::AudioPacket packet;
    packet.track = backtrack::AudioTrack::Microphone;
    packet.format = std::make_shared<backtrack::WaveFormatBlob>(backtrack::canonicalAudioFormat());
    packet.frameCount = 480;
    packet.pts100ns = 12345;
    packet.bytes.resize(packet.frameCount * format.blockAlign);
    const float rmsDb = static_cast<float>(20.0 * std::log10(std::sqrt((0.02 * 0.02 + 0.01 * 0.01) / 2)));
    backtrack::MicrophoneAgc agc;
    backtrack::MicrophoneAgc expectedAgc;
    for (int block = 0; block < 3; ++block) {
        for (size_t offset = 0; offset < packet.bytes.size(); offset += sizeof(float)) {
            const float sample = (offset / sizeof(float)) % 2 == 0 ? 0.02f : -0.01f;
            std::memcpy(packet.bytes.data() + offset, &sample, sizeof(sample));
        }
        const float gain = expectedAgc.gainFor(rmsDb, 0.010f);
        backtrack::applyMicrophoneAgc(packet, format, agc);
        for (size_t offset = 0; offset < packet.bytes.size(); offset += sizeof(float)) {
            float sample = 0.0f;
            std::memcpy(&sample, packet.bytes.data() + offset, sizeof(sample));
            const float input = (offset / sizeof(float)) % 2 == 0 ? 0.02f : -0.01f;
            expect(std::fabs(sample - input * gain) < 0.000001f,
                   "microphone AGC must preserve RMS gain, block duration, and envelope continuity");
        }
    }
    expect(packet.pts100ns == 12345 && packet.frameCount == 480 &&
           packet.track == backtrack::AudioTrack::Microphone &&
           backtrack::parseAudioFormat(*packet.format) == format,
           "microphone AGC must preserve packet metadata");
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
        testBlockRms();
        testMicrophoneAgcUsesBlockRms();
        testLatePacketConsumed();
        testLatePacketConsumedMicrophone();
        testConsumerDrainsBeforeExit();
    } catch (const std::exception& error) {
        std::cerr << "RecorderControllerShutdownOrderingTests: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
