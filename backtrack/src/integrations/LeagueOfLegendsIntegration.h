#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace backtrack {

class LeagueOfLegendsIntegration {
public:
    using KillCallback = std::function<void()>;

    explicit LeagueOfLegendsIntegration(KillCallback killCallback);
    ~LeagueOfLegendsIntegration();

    void start();
    void stop();
    bool running() const;

private:
    void workerLoop();

    KillCallback killCallback_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
};

} // namespace backtrack
