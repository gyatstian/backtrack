#pragma once

#include "core/SpscQueue.h"
#include "core/Types.h"

#include <Windows.h>

namespace backtrack {

// Drains both audio queues until the WASAPI producers have finished and every
// packet is consumed. The writer exits only on the producers-finished flag,
// never on a generic stop request: a producer holding a packet when the stop
// lands must have time to enqueue it before the writer exits.
//
// stopPipeline sets producersFinished only after joining both producer threads,
// so no new packets can arrive after that point. The writer then drains the
// remaining packets and returns.
template <typename WriteFn>
void drainAudioQueues(
    SpscQueue<AudioPacket>& systemQueue,
    SpscQueue<AudioPacket>& microphoneQueue,
    const std::atomic<bool>& producersFinished,
    HANDLE audioEvent,
    WriteFn&& write) {
    while (!producersFinished.load(std::memory_order_acquire) ||
           systemQueue.size() > 0 || microphoneQueue.size() > 0) {
        AudioPacket packet;
        bool wrotePacket = false;
        while (systemQueue.tryPop(packet)) {
            write(std::move(packet));
            wrotePacket = true;
        }
        while (microphoneQueue.tryPop(packet)) {
            write(std::move(packet));
            wrotePacket = true;
        }
        if (!wrotePacket && audioEvent) {
            WaitForSingleObject(audioEvent, 16);
        }
    }
}

} // namespace backtrack
