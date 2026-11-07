#include "ui/MainWindow.h"
#include "ui/UiHelpers.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace backtrack {

struct MainWindowNotificationTestAccess {
    static void attach(MainWindow& window, HWND root, HWND status) {
        window.window_ = root;
        window.status_ = status;
    }
    static bool busyFeedback(MainWindow& window) {
        window.controllerActionPending_.store(true);
        MainWindow::ControllerAction action;
        const bool queued = window.queueControllerAction(std::move(action), L"Recorder is busy");
        return !queued && window.controllerActionPending_.load() &&
            window.statusText_ == L"Recorder is busy" && readText(window.status_) == L"Recorder is busy";
    }
};

} // namespace backtrack

namespace {

using namespace std::chrono_literals;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    bool busyReturned = false;
    bool timedOut = false;

    void block() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }
};

struct StatusWindow {
    HWND root = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 800, 600,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND status = root ? CreateWindowExW(0, L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE,
                                         0, 0, 500, 24, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    StatusWindow() {
        if (!status && root) DestroyWindow(root);
        expect(root && status, "could not create the status window");
    }
    ~StatusWindow() { DestroyWindow(root); }
};

Gate* gate = nullptr;

std::vector<uint8_t> blockedLoader(const std::filesystem::path&) {
    gate->block();
    return {};
}

BOOL WINAPI blockedPlayer(LPCWSTR sound, HMODULE, DWORD) {
    if (!sound) gate->block();
    return TRUE;
}

BOOL WINAPI successfulPlayer(LPCWSTR, HMODULE, DWORD) {
    return TRUE;
}

void testBusyFeedbackWhileSoundBlocked(bool blockLoad) {
    backtrack::RecorderController controller;
    backtrack::SettingsStore store(std::filesystem::temp_directory_path() / L"backtrack_notification_test.ini");
    backtrack::AppSettings settings;
    settings.notificationSoundVolumePercent = 1;
    StatusWindow status;
    backtrack::MainWindow window(controller, store, settings, backtrack::SettingsStore::LoadStatus::Missing);
    backtrack::MainWindowNotificationTestAccess::attach(window, status.root, status.status);

    Gate state;
    gate = &state;
    std::thread worker([&] {
        if (blockLoad) {
            backtrack::playActionIndicator(MB_OK, 100, L"blocked.wav", blockedLoader, successfulPlayer);
        } else {
            backtrack::playActionIndicator(MB_ICONHAND, 100, {}, nullptr, blockedPlayer);
        }
    });
    bool entered;
    {
        std::unique_lock lock(state.mutex);
        entered = state.changed.wait_for(lock, 3s, [&] { return state.entered; });
        if (!entered) {
            state.released = true;
            state.changed.notify_all();
        }
    }
    if (!entered) {
        worker.join();
        gate = nullptr;
        expect(false, "sound worker did not reach the blocked operation");
    }

    // Release the worker on timeout so a regression fails instead of hanging the suite.
    std::thread watchdog([&] {
        std::unique_lock lock(state.mutex);
        if (!state.changed.wait_for(lock, 2s, [&] { return state.busyReturned; })) {
            state.timedOut = true;
            state.released = true;
            state.changed.notify_all();
        }
    });
    const bool reportedBusy = backtrack::MainWindowNotificationTestAccess::busyFeedback(window);
    {
        std::scoped_lock lock(state.mutex);
        state.busyReturned = true;
        state.released = true;
        state.changed.notify_all();
    }
    watchdog.join();
    worker.join();
    gate = nullptr;
    expect(reportedBusy, "busy action was not rejected with its status");
    expect(!state.timedOut, blockLoad
        ? "busy feedback waited for the custom WAV loader"
        : "busy feedback waited for the occupied playback state");
}

} // namespace

int main() {
    try {
        testBusyFeedbackWhileSoundBlocked(true);
        testBusyFeedbackWhileSoundBlocked(false);
        std::cout << "MainWindowNotificationTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MainWindowNotificationTests failed: " << error.what() << '\n';
        return 1;
    }
}
