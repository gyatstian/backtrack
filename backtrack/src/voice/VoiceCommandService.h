#pragma once

#include "core/Types.h"

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace backtrack {

inline constexpr UINT kVoiceCommandMessage = WM_APP + 6;

struct VoiceCommandStats {
    bool enabled = false;
    bool ready = false;
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t initializationFailures = 0;
    std::wstring status;
};

class VoiceCommandService {
public:
    VoiceCommandService() = default;
    ~VoiceCommandService();

    void configure(HWND window, GameIntegrationSettings::VoiceCommandMode mode, bool replayEnabled);
    void stop();
    VoiceCommandStats stats() const;

private:
    void run(GameIntegrationSettings::VoiceCommandMode mode);
    void setStatus(bool ready, std::wstring status);

    mutable std::mutex stateMutex_;
    HWND window_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::thread thread_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> ready_{false};
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<uint64_t> initializationFailures_{0};
    std::wstring status_ = L"disabled";
    // Last configure() inputs, so an unchanged reconfigure (every save
    // completion) skips the SAPI teardown/rebuild. UI thread only.
    HWND configuredWindow_ = nullptr;
    GameIntegrationSettings::VoiceCommandMode configuredMode_ = GameIntegrationSettings::VoiceCommandMode::Disabled;
    bool configuredReplayEnabled_ = false;
};

} // namespace backtrack
