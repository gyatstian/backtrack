#pragma once

#include "core/Types.h"
#include "ui/UiIds.h"

#include <Windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace backtrack {

using namespace ui_ids;

std::wstring readText(HWND control);
std::wstring trimText(std::wstring value);
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
bool isSafeBareHotkey(uint32_t virtualKey);
uint32_t supportedHotkeyModifiers(uint32_t modifiers);
bool isSafeGlobalHotkey(uint32_t modifiers, uint32_t virtualKey);
uint32_t currentModifierState();
bool isControlDown();
bool shellExecuteSucceeded(HINSTANCE result);
std::wstring keyName(uint32_t virtualKey);
std::wstring hotkeyDisplay(uint32_t modifiers, uint32_t virtualKey);
const wchar_t* yesNo(bool value);
const wchar_t* captureBackendDisplayName(CaptureBackend backend);
std::wstring actionFailureStatus(const wchar_t* summary, const EncoderCapabilities& caps, const RecordingStats& stats);
std::wstring withHotkeyWarning(std::wstring status, bool hotkeysOk, const std::wstring& hotkeyError);
void playActionIndicator(
    UINT type,
    uint32_t volumePercent = 100,
    const std::filesystem::path& customSuccessSoundPath = {});
std::wstring thumbnailCacheKey(const std::filesystem::path& path);
std::wstring executableKey(const std::filesystem::path& path);
std::wstring appNameFromPath(const std::filesystem::path& path);
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
bool resolutionPresetSize(ResolutionMode mode, uint32_t& width, uint32_t& height);

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