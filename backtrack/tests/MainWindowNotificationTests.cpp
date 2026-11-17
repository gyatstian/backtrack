#include "ui/MainWindow.h"
#include "ui/UiHelpers.h"

#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <cstring>
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

std::vector<uint8_t> loadedWav;
std::vector<uint8_t>* capturedWav = nullptr;

std::vector<uint8_t> fixtureLoader(const std::filesystem::path&) {
    return loadedWav;
}

uint32_t readUInt32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

void appendUInt16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void appendUInt32(std::vector<uint8_t>& out, uint32_t value) {
    appendUInt16(out, static_cast<uint16_t>(value & 0xffff));
    appendUInt16(out, static_cast<uint16_t>(value >> 16));
}

void appendFourCc(std::vector<uint8_t>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

// 44 bytes of header plus the given sample bytes and any chunks that follow the
// data chunk. Plausibility checks in the loader must accept it.
std::vector<uint8_t> makeWav(uint16_t formatTag, uint16_t bitsPerSample,
                             const std::vector<uint8_t>& samples,
                             const std::vector<uint8_t>& trailing) {
    std::vector<uint8_t> wav;
    appendFourCc(wav, "RIFF");
    appendUInt32(wav, 0);
    appendFourCc(wav, "WAVE");
    appendFourCc(wav, "fmt ");
    appendUInt32(wav, 16);
    appendUInt16(wav, formatTag);
    appendUInt16(wav, 1);
    appendUInt32(wav, 8000);
    appendUInt32(wav, 8000u * bitsPerSample / 8);
    appendUInt16(wav, static_cast<uint16_t>(bitsPerSample / 8));
    appendUInt16(wav, bitsPerSample);
    appendFourCc(wav, "data");
    appendUInt32(wav, static_cast<uint32_t>(samples.size()));
    wav.insert(wav.end(), samples.begin(), samples.end());
    wav.insert(wav.end(), trailing.begin(), trailing.end());
    const uint32_t riffSize = static_cast<uint32_t>(wav.size() - 8);
    wav[4] = static_cast<uint8_t>(riffSize & 0xff);
    wav[5] = static_cast<uint8_t>((riffSize >> 8) & 0xff);
    wav[6] = static_cast<uint8_t>((riffSize >> 16) & 0xff);
    wav[7] = static_cast<uint8_t>((riffSize >> 24) & 0xff);
    return wav;
}

// Both buffers production can hand to the platform player are RIFF files whose
// header states their length, so copy exactly that many bytes.
BOOL WINAPI capturingPlayer(LPCWSTR sound, HMODULE, DWORD) {
    if (sound && capturedWav) {
        uint8_t header[44]{};
        std::memcpy(header, sound, sizeof(header));
        std::vector<uint8_t> bytes(8 + readUInt32(header + 4));
        std::memcpy(bytes.data(), sound, bytes.size());
        *capturedWav = std::move(bytes);
    }
    return TRUE;
}

void testCustomSoundScalesOnlyItsSampleData() {
    std::vector<uint8_t> trailing;
    appendFourCc(trailing, "LIST");
    appendUInt32(trailing, 8);
    const uint8_t info[] = {'I', 'N', 'F', 'O', 't', 'e', 's', 't'};
    trailing.insert(trailing.end(), std::begin(info), std::end(info));

    loadedWav = makeWav(1, 16, {0x10, 0x27, 0xf0, 0xd8}, trailing);
    std::vector<uint8_t> played;
    capturedWav = &played;
    backtrack::playActionIndicator(MB_OK, 50, L"custom.wav", fixtureLoader, capturingPlayer);
    capturedWav = nullptr;
    expect(played.size() == loadedWav.size(), "the custom WAV was not played");
    expect(played[44] == 0x88 && played[45] == 0x13 && played[46] == 0x78 && played[47] == 0xec,
           "16-bit samples were not scaled by 50%");
    expect(std::equal(played.end() - trailing.size(), played.end(), trailing.begin()),
           "a chunk after the sample data was rewritten by volume scaling");
}

void testCustom8BitSoundScalesFromItsCentre() {
    loadedWav = makeWav(1, 8, {200, 80, 128, 255, 0}, {});
    std::vector<uint8_t> played;
    capturedWav = &played;
    backtrack::playActionIndicator(MB_OK, 50, L"custom.wav", fixtureLoader, capturingPlayer);
    capturedWav = nullptr;
    expect(played.size() == loadedWav.size(), "the 8-bit custom WAV was not played");
    expect(played[44] == 164 && played[45] == 104 && played[46] == 128 &&
               played[47] == 192 && played[48] == 64,
           "8-bit samples were not halved around 128");
}

void testUnattenuableCustomSoundFallsBackToBuiltIn() {
    loadedWav = makeWav(3, 32, {0, 0, 0, 0, 0, 0, 0x80, 0x3f}, {});
    std::vector<uint8_t> builtIn;
    capturedWav = &builtIn;
    backtrack::playActionIndicator(MB_OK, 50, {}, nullptr, capturingPlayer);
    std::vector<uint8_t> played;
    capturedWav = &played;
    backtrack::playActionIndicator(MB_OK, 50, L"custom.wav", fixtureLoader, capturingPlayer);
    capturedWav = nullptr;
    expect(!builtIn.empty() && played == builtIn,
           "an unattenuable custom sound did not fall back to the built-in indicator");
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
        testCustomSoundScalesOnlyItsSampleData();
        testCustom8BitSoundScalesFromItsCentre();
        testUnattenuableCustomSoundFallsBackToBuiltIn();
        std::cout << "MainWindowNotificationTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MainWindowNotificationTests failed: " << error.what() << '\n';
        return 1;
    }
}
