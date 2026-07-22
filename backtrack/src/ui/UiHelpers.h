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
uint32_t readUIntControl(HWND control, uint32_t fallback);
std::wstring bytesToText(uintmax_t bytes);
bool isButtonId(WPARAM id);
bool isSoundSeparationMuteButtonId(int controlId);
bool isSoundSeparationRemoveButtonId(int controlId);
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
void playActionIndicator(UINT type);
std::wstring thumbnailCacheKey(const std::filesystem::path& path);
std::wstring executableKey(const std::filesystem::path& path);
std::wstring appNameFromPath(const std::filesystem::path& path);
HICON loadExecutableIcon(const std::filesystem::path& path);
HBITMAP loadShellThumbnail(const std::filesystem::path& path, int width, int height);
void drawBitmapFit(HDC dc, HBITMAP bitmap, const RECT& target);
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