#pragma once

#include <Windows.h>
#include <avrt.h>
#include <dxgi.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace backtrack {

// One entry per active display, in EnumDisplayMonitors order so index aligns
// with monitorFromIndex / settings.monitorIndex.
struct MonitorEnumEntry {
    uint32_t index = 0;
    std::wstring deviceName; // e.g. "\\.\DISPLAY1"
    uint32_t width = 0;
    uint32_t height = 0;
    bool primary = false;
};

inline constexpr wchar_t kBacktrackMainWindowClassName[] = L"BacktrackMainWindow";
inline constexpr wchar_t kBacktrackSingleInstanceMutexName[] = L"Local\\Backtrack.SingleInstance";
inline constexpr wchar_t kBacktrackStartupArgument[] = L"--startup";

struct DxgiOutputLocation {
    uint32_t adapterIndex = UINT32_MAX;
    uint32_t outputIndex = UINT32_MAX;

    bool valid() const {
        return adapterIndex != UINT32_MAX && outputIndex != UINT32_MAX;
    }
};

std::filesystem::path localAppDataPath();
std::filesystem::path defaultClipDirectory();
std::wstring makeTimestampedFileName(const wchar_t* prefix, const wchar_t* extension);
std::wstring foregroundApplicationName();
// Returns true when the foreground window covers an entire monitor (borderless
// / exclusive fullscreen on any display). Used by Discord Rich Presence to
// restrict activity updates to fullscreen games.
bool foregroundWindowIsFullscreen();
void setThreadDescriptionSafe(const wchar_t* description);
// Registers the calling thread with the MMCSS task named by `taskName`
// ("Capture", "Games", ...) and raises it to `priority` within that profile.
HANDLE enableMmcssForThread(const wchar_t* taskName, AVRT_PRIORITY priority);
HANDLE enableMmcssForCaptureThread();
// Encode loop: "Games" profile keeps encoding above background load so the
// drop-newest frame queue does not shed the freshest replay frames.
HANDLE enableMmcssForEncodeThread();
void disableMmcssForThread(HANDLE handle);

// Raises the process-wide system timer resolution to its finest supported
// period (typically 1ms) for the lifetime of the guard, then restores it.
// Scope this to an active capture/encode session only: the setting is global
// and keeping it raised while idle increases power draw. Coarse waits (e.g.
// WaitForSingleObject) quantize to the current timer period, so a 60fps
// 16.67ms cadence aliases toward ~30fps under the 15.6ms default.
class HighResolutionTimerScope {
public:
    HighResolutionTimerScope();
    ~HighResolutionTimerScope();

    HighResolutionTimerScope(const HighResolutionTimerScope&) = delete;
    HighResolutionTimerScope& operator=(const HighResolutionTimerScope&) = delete;
    HighResolutionTimerScope(HighResolutionTimerScope&&) = delete;
    HighResolutionTimerScope& operator=(HighResolutionTimerScope&&) = delete;

    bool active() const { return active_; }

private:
    bool active_ = false;
    unsigned int period_ = 0;
};

// Creates a high-resolution waitable timer (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
// Windows 10 1803+). Returns nullptr on failure; caller falls back to coarse waits.
HANDLE createHighResolutionWaitableTimer();
// Waits up to timeoutMs using a high-resolution timer, immune to the global
// system timer period. Returns true if it waited the requested duration,
// false if the timer was unusable (caller should fall back).
bool waitHighResolution(HANDLE timer, uint32_t timeoutMs);
std::wstring moduleFilePath();
bool updateWindowsStartupRegistration(bool enabled);
void pruneStaleMicrophoneConsentEntries();
HMONITOR monitorFromIndex(uint32_t index);
// Enumerates active displays with native pixel size and primary flag.
std::vector<MonitorEnumEntry> enumerateMonitors();
// True when the output driving this monitor reports advanced color active
// (HDR: ST.2084/PQ or linear scRGB). Best-effort: returns false on legacy
// systems or when the output cannot be resolved, so callers treat SDR as the
// safe default.
bool monitorHasAdvancedColor(HMONITOR monitor);
HMONITOR focusedMonitorOrFallback(uint32_t fallbackIndex);
HMONITOR cursorMonitorOrFallback(uint32_t fallbackIndex);
// Returns DXGI output index on the given adapter for HMONITOR, or UINT32_MAX if none.
uint32_t dxgiOutputIndexForMonitor(IDXGIAdapter* adapter, HMONITOR monitor);
// Finds monitor's output across every DXGI adapter. Desktop Duplication needs a
// D3D device created on this adapter.
DxgiOutputLocation dxgiOutputForMonitor(HMONITOR monitor);
// Current encoder factory supports these adapter vendors only.
bool dxgiAdapterSupportsHardwareEncode(uint32_t adapterIndex);
// Returns preferred adapter when it can encode, otherwise first supported one.
uint32_t dxgiHardwareEncoderAdapterOr(uint32_t preferredAdapterIndex);
UINT backtrackActivationMessage();

// Strips leading and trailing whitespace. Inline rather than exported:
// SettingsClipGuardTests links SettingsStore.cpp without Win32Util.cpp (DXGI,
// avrt, winmm), and an out-of-line definition would drag that in for a one-liner.
inline std::wstring trimText(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && iswspace(value.back())) {
        value.pop_back();
    }
    return value;
}

// Case- and separator-insensitive executable path key. The sound-separation
// mute list is keyed by it on both sides - WasapiCapture keys the live session,
// RecorderController and the UI key the user's request - so those keys must
// match. Changing what this normalizes silently stops muting, with no error
// logged. Do not "improve" it (env expansion, 8.3 names) without changing every
// key at once.
inline std::wstring normalizedPathKey(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

} // namespace backtrack
