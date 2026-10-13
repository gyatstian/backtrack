#pragma once

#include "core/Types.h"
#include "ui/UiIds.h"

#include <Windows.h>

#include <shobjidl.h>

#include <filesystem>
#include <string>
#include <vector>

namespace backtrack {

using namespace ui_ids;

std::wstring readText(HWND control);
bool setText(HWND control, const std::wstring& value);
void redrawWindowAndChildren(HWND window, bool updateNow = false);
bool moveWindowIfChanged(HWND control, int x, int y, int width, int height);
bool showWindowIfHidden(HWND control);
bool setListItemHeightIfChanged(HWND list, int& cachedHeight, int height);

// One child-window reposition. Used with applyWindowMoves().
struct WindowMove {
    HWND control = nullptr;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// Applies a batch of moves in one DeferWindowPos sequence so relayouts cost
// one layout pass instead of one internal layout per SetWindowPos. Falls back
// to individual SetWindowPos calls if deferring fails; moves are idempotent,
// so the fallback is always safe.
bool applyWindowMoves(const std::vector<WindowMove>& moves);
uint32_t readUIntControl(HWND control, uint32_t fallback);
// One {enum value, display label} row for a settings combo box. The page
// builders fill from the same table the reader maps back through, so inserting
// a value in the middle of an enum cannot silently misread saves the way a raw
// static_cast<Enum>(selected) does. Tables live here (not core/Types.h) because
// these spellings are display labels; the file format keeps its own xxxName
// pairs in settings/SettingsStore.h.
struct EnumComboEntry {
    int value;
    const wchar_t* label;
};

inline constexpr EnumComboEntry kResolutionPresetComboEntries[] = {
    {static_cast<int>(ResolutionMode::P240), L"240p"},
    {static_cast<int>(ResolutionMode::P480), L"480p"},
    {static_cast<int>(ResolutionMode::P720), L"720p"},
    {static_cast<int>(ResolutionMode::P1080), L"1080p"},
    {static_cast<int>(ResolutionMode::P2K), L"2K"},
    {static_cast<int>(ResolutionMode::P4K), L"4K"},
    {static_cast<int>(ResolutionMode::Custom), L"Custom size"},
};
inline constexpr EnumComboEntry kVideoCodecComboEntries[] = {
    {static_cast<int>(VideoCodec::H264), L"H.264"},
    {static_cast<int>(VideoCodec::Hevc), L"HEVC"},
};
inline constexpr EnumComboEntry kEncoderPresetComboEntries[] = {
    {static_cast<int>(EncoderPreset::P1), L"P1 fastest"},
    {static_cast<int>(EncoderPreset::P2), L"P2 faster"},
    {static_cast<int>(EncoderPreset::P3), L"P3 fast"},
    {static_cast<int>(EncoderPreset::P4), L"P4 medium"},
    {static_cast<int>(EncoderPreset::P5), L"P5 slow"},
    {static_cast<int>(EncoderPreset::P6), L"P6 slower"},
    {static_cast<int>(EncoderPreset::P7), L"P7 slowest"},
};
inline constexpr EnumComboEntry kEncoderModeComboEntries[] = {
    {static_cast<int>(EncoderMode::HighQuality), L"High quality"},
    {static_cast<int>(EncoderMode::LowLatency), L"Low latency"},
    {static_cast<int>(EncoderMode::UltraLowLatency), L"Ultra low latency"},
    {static_cast<int>(EncoderMode::Lossless), L"Lossless"},
    {static_cast<int>(EncoderMode::UltraHighQuality), L"Ultra high quality"},
};
inline constexpr EnumComboEntry kEncoderProfileComboEntries[] = {
    {static_cast<int>(EncoderProfile::LowestGpu), L"Lowest GPU"},
    {static_cast<int>(EncoderProfile::Balanced), L"Balanced"},
    {static_cast<int>(EncoderProfile::Custom), L"Custom"},
};
inline constexpr EnumComboEntry kEncoderMultipassComboEntries[] = {
    {static_cast<int>(EncoderMultipass::Disabled), L"Disabled"},
    {static_cast<int>(EncoderMultipass::QuarterResolution), L"Quarter resolution"},
    {static_cast<int>(EncoderMultipass::FullResolution), L"Full resolution"},
};
inline constexpr EnumComboEntry kGpuAdaptiveComboEntries[] = {
    {static_cast<int>(GpuAdaptiveMode::Disabled), L"Disabled"},
    {static_cast<int>(GpuAdaptiveMode::Conservative), L"Conservative"},
    {static_cast<int>(GpuAdaptiveMode::Aggressive), L"Aggressive"},
};
inline constexpr EnumComboEntry kCaptureBackendComboEntries[] = {
    {static_cast<int>(CaptureBackend::WindowsGraphicsCapture), L"WGC"},
    {static_cast<int>(CaptureBackend::DesktopDuplication), L"DXGI"},
};
inline constexpr EnumComboEntry kVoiceCommandComboEntries[] = {
    {static_cast<int>(GameIntegrationSettings::VoiceCommandMode::Disabled), L"Off"},
    {static_cast<int>(GameIntegrationSettings::VoiceCommandMode::ClipThat), L"Backtrack clip that"},
    {static_cast<int>(GameIntegrationSettings::VoiceCommandMode::RecordVideo), L"Record video"},
};
inline constexpr EnumComboEntry kDiscordPresenceComboEntries[] = {
    {static_cast<int>(GameIntegrationSettings::DiscordRichPresenceMode::Always), L"Always"},
    {static_cast<int>(GameIntegrationSettings::DiscordRichPresenceMode::FullscreenOnly), L"Only on fullscreen apps/games"},
    {static_cast<int>(GameIntegrationSettings::DiscordRichPresenceMode::Off), L"Never"},
};
inline constexpr EnumComboEntry kLogLevelComboEntries[] = {
    {static_cast<int>(LogLevel::Trace), L"Trace"},
    {static_cast<int>(LogLevel::Debug), L"Debug"},
    {static_cast<int>(LogLevel::Info), L"Info"},
    {static_cast<int>(LogLevel::Warning), L"Warn"},
    {static_cast<int>(LogLevel::Error), L"Error"},
};

// Fills a fresh combo from a table and selects the row holding currentValue.
// An unknown value falls back to the first row rather than leaving no selection.
template <size_t Count>
void fillEnumCombo(HWND combo, const EnumComboEntry (&entries)[Count], int currentValue) {
    if (!combo) {
        return;
    }
    int selected = 0;
    for (size_t index = 0; index < Count; ++index) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entries[index].label));
        if (entries[index].value == currentValue) {
            selected = static_cast<int>(index);
        }
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
}

// Maps the combo selection back through the same table. Returns false (and
// keeps value) when nothing valid is selected, so CB_ERR can never silently
// become enum value 0 the way the old selected==1?B:A reads did.
template <size_t Count>
bool readEnumCombo(HWND combo, const EnumComboEntry (&entries)[Count], int& value) {
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected < 0 || static_cast<size_t>(selected) >= Count) {
        return false;
    }
    value = entries[static_cast<size_t>(selected)].value;
    return true;
}

// shortcut: bytesToText/durationToText stay UI-local until a non-ui caller needs them (then join captureBackendDisplayName in platform/).
std::wstring bytesToText(uintmax_t bytes);
bool isButtonId(WPARAM id);
bool isSoundSeparationMuteButtonId(int controlId);
bool isSoundSeparationRemoveButtonId(int controlId);
bool isAutoRecordRemoveButtonId(int controlId);
size_t soundSeparationIndexFromButtonId(int controlId, int baseId);
bool isSettingsControlId(int controlId);
std::wstring durationToText(uint64_t duration100ns);
std::wstring tagsToText(const std::vector<std::wstring>& tags);
int comboIndexForDevice(const std::vector<AudioDeviceInfo>& devices, const std::wstring& selectedId);
std::wstring selectedDeviceId(HWND combo, const std::vector<AudioDeviceInfo>& devices);
bool isModifierKey(WPARAM key);
uint32_t currentModifierState();
bool isControlDown();
bool shellExecuteSucceeded(HINSTANCE result);
std::wstring keyName(uint32_t virtualKey);
std::wstring hotkeyDisplay(uint32_t modifiers, uint32_t virtualKey);
const wchar_t* yesNo(bool value);
std::wstring actionFailureStatus(const wchar_t* summary, const EncoderCapabilities& caps, const RecordingStats& stats);
std::wstring withHotkeyWarning(std::wstring status, bool hotkeysOk, const std::wstring& hotkeyError);
void playActionIndicator(
    UINT type,
    uint32_t volumePercent = 100,
    const std::filesystem::path& customSuccessSoundPath = {});
std::wstring thumbnailCacheKey(const std::filesystem::path& path);
std::wstring appNameFromPath(const std::filesystem::path& path);
// Shared file/folder pickers so the three manual-app/sound pickers and the two
// folder browsers share one dialog core instead of copied boilerplate.
bool pickFileForOpen(
    HWND owner,
    const wchar_t* title,
    const COMDLG_FILTERSPEC* filters,
    UINT filterCount,
    const wchar_t* defaultExtension,
    std::filesystem::path& out);
bool pickFolder(HWND owner, const wchar_t* title, std::wstring& out);
HICON loadExecutableIcon(const std::filesystem::path& path);
HBITMAP loadShellThumbnail(const std::filesystem::path& path, int width, int height);
void drawBitmapFit(HDC dc, HBITMAP bitmap, const RECT& target);
// drawBitmapFit with a caller-owned cache of the scaled copy: the first call
// for a given target size scales once (same HALFTONE mode as drawBitmapFit),
// later calls with an unchanged size plain BitBlt the cached copy. On any GDI
// failure it falls back to drawBitmapFit, so output is never worse.
void drawBitmapFitCached(
    HDC dc,
    HBITMAP bitmap,
    HBITMAP& scaledBitmap,
    int& scaledWidth,
    int& scaledHeight,
    const RECT& target);
int galleryItemHeightForWidth(int listWidth);

class ScopedRedrawLock {
public:
    explicit ScopedRedrawLock(HWND window);
    ~ScopedRedrawLock();
    ScopedRedrawLock(const ScopedRedrawLock&) = delete;
    ScopedRedrawLock& operator=(const ScopedRedrawLock&) = delete;

private:
    HWND window_ = nullptr;
};

class ClipFileDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override;
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override;

private:
    LONG references_ = 1;
};

} // namespace backtrack