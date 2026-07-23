#pragma once

#include "core/Types.h"

#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace backtrack {

using AudioPacketCallback = std::function<void(AudioPacket&&)>;

enum class ProcessLoopbackMode {
    IncludeTargetProcessTree,
    ExcludeTargetProcessTree,
};

class WasapiCapture {
public:
    WasapiCapture() = default;
    ~WasapiCapture();

    static std::vector<AudioDeviceInfo> enumerateDevices(AudioTrack track);
    static std::vector<AudioSessionAppInfo> enumerateAudioSessionApps();
    static std::vector<AudioSessionAppInfo> enumerateOpenApps();
    // Mutes active render sessions whose image path is in mutedExecutableKeys (lowercase paths).
    // Restores any previously applied mutes that are no longer requested. Returns muted process count.
    static uint32_t applySessionMutesForExecutables(const std::vector<std::wstring>& mutedExecutableKeys);
    static void clearSessionMutes();

    bool start(AudioTrack track, const std::wstring& deviceId, AudioPacketCallback callback);
    bool startProcessLoopback(uint32_t processId, AudioPacketCallback callback);
    bool startProcessLoopback(AudioTrack track, uint32_t processId, ProcessLoopbackMode mode, AudioPacketCallback callback);
    void stop();
    bool running() const { return running_.load(); }
    WaveFormatBlob format() const;

private:
    struct CaptureOptions {
        AudioTrack track = AudioTrack::System;
        std::wstring deviceId;
        uint32_t processId = 0;
        bool processLoopback = false;
        ProcessLoopbackMode processLoopbackMode = ProcessLoopbackMode::IncludeTargetProcessTree;
    };

    bool start(CaptureOptions options, AudioPacketCallback callback);
    void captureThread(CaptureOptions options, AudioPacketCallback callback, std::promise<bool> startedPromise);

    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    HANDLE wakeEvent_ = nullptr;
    mutable std::mutex formatMutex_;
    WaveFormatBlob format_;
};

} // namespace backtrack
