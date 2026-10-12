#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace backtrack {

struct LeagueIntegrationStats {
    bool gameAvailable = false;
    std::wstring lastError;
};

class LeagueOfLegendsIntegration {
public:
    using KillCallback = std::function<void()>;

    explicit LeagueOfLegendsIntegration(KillCallback killCallback);
    ~LeagueOfLegendsIntegration();

    void start();
    void stop();
    LeagueIntegrationStats stats() const;

private:
    void workerLoop();
    void setStats(bool gameAvailable, std::wstring lastError);

    KillCallback killCallback_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    mutable std::mutex statsMutex_;
    bool gameAvailable_ = false;
    std::wstring lastError_;
};

} // namespace backtrack
