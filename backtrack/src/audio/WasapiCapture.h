#pragma once

#include "audio/SessionMuteStore.h"
#include "core/Types.h"

#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct IMMDeviceEnumerator;

namespace backtrack {

using AudioPacketCallback = std::function<void(AudioPacket&&)>;

// Installs the pooled allocator that AudioPacket::bytes draws from, so the
// WASAPI capture thread does no CRT heap work per device callback. Must run
// before any AudioPacket is constructed: a pmr vector binds its resource at
// construction, and RecorderController builds its audio queues (257 packets
// each) in its constructor. wWinMain calls this before constructing Application.
void ensureAudioPacketPool();

class WasapiCapture {
public:
    WasapiCapture() = default;
    ~WasapiCapture();

    static std::vector<AudioDeviceInfo> enumerateDevices(AudioTrack track);
    static std::vector<AudioSessionAppInfo> enumerateAudioSessionApps();
    static std::vector<AudioSessionAppInfo> enumerateOpenApps();
    // Mutes every live render session whose image path is in mutedExecutableKeys
    // (normalized paths), one mute per session instance, and restores sessions
    // that are no longer requested. Returns what the exclusion actually
    // established: callers must not record system audio while
    // exclusionEstablished() is false.
    static SessionMuteApplyResult applySessionMutesForExecutables(const std::vector<std::wstring>& mutedExecutableKeys);
    // Restores every mute this process applied. Records that could not be
    // verified stay journaled for the next launch.
    static SessionMuteRestoreResult clearSessionMutes();
    // Startup sweep: restores foreign render sessions that a previous crashed
    // session left muted (tracked in a journal under the local app-data
    // directory). No-op when no journal exists.
    static void restoreOrphanedSessionMutes();
    // Crash-path restore (unhandled exception / terminate / ctrl handler).
    // Best effort and noexcept; skips when another thread holds the registry.
    static void emergencyRestoreSessionMutes() noexcept;

    // Ownership of the mute journal and of the live mutes this process applied.
    // Only the process that won the single-instance mutex may restore mutes or
    // rewrite the journal: a rejected second launch still constructs and
    // destroys a controller, and its restore would unmute the live instance's
    // exclusions. Default false, so a losing launch restores nothing.
    static void setSessionMuteOwnership(bool owned) noexcept;

    bool start(AudioTrack track, const std::wstring& deviceId, AudioPacketCallback callback);
    void stop();
    bool running() const { return running_.load(); }

private:
    friend struct WasapiCaptureTestAccess;

    struct CaptureOptions {
        AudioTrack track = AudioTrack::System;
        std::wstring deviceId;
        std::function<HRESULT(IMMDeviceEnumerator**)> createEnumerator;
    };

    // Test-only seam: the IMMDeviceEnumerator the session-mute enumeration
    // creates. Empty in production, which then creates the real one.
    static std::function<HRESULT(IMMDeviceEnumerator**)>& sessionMuteEnumeratorFactory();

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
