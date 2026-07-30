#pragma once

#include "core/Types.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace backtrack {

// Minimal Discord Rich Presence integration. Connects to the local Discord
// client over the named-pipe IPC protocol, updates the activity state while
// Backtrack is capturing, and exposes a button linking to the project's GitHub
// repository. The activity details read "Clipping <game name>" where the game
// name is derived from the currently focused foreground application (the same
// source used when naming saved replays).
class DiscordRichPresence {
public:
    DiscordRichPresence() = default;
    ~DiscordRichPresence();

    DiscordRichPresence(const DiscordRichPresence&) = delete;
    DiscordRichPresence& operator=(const DiscordRichPresence&) = delete;

    void start(GameIntegrationSettings::DiscordRichPresenceMode mode);
    void stop();
    bool running() const;

private:
    void workerLoop();
    bool connect();
    void disconnect();
    bool handshake();
    bool sendFrame(int opcode, const std::string& payload);
    bool drainInbox();
    bool updateActivity(const std::string& gameName);

    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<GameIntegrationSettings::DiscordRichPresenceMode> mode_{
        GameIntegrationSettings::DiscordRichPresenceMode::Off};
    void* pipe_ = nullptr;
};

} // namespace backtrack
