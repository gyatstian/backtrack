#include "ui/MainWindow.h"

#include "audio/WasapiCapture.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"
#include "resource.h"
#include "settings/SettingsStore.h"

#include <CommCtrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace backtrack {

namespace {

constexpr int kTabId = 10;
constexpr int kTabButtonBaseId = 20;
constexpr int kStatusId = 11;
constexpr int kFpsEditId = 80;
constexpr int kResolutionModeComboId = 81;
constexpr int kWidthEditId = 82;
constexpr int kHeightEditId = 83;
constexpr int kSystemAudioCheckId = 84;
constexpr int kMicrophoneCheckId = 85;
constexpr int kOutputDeviceComboId = 86;
constexpr int kInputDeviceComboId = 87;
constexpr int kStartWithWindowsCheckId = 88;
constexpr int kExitToTrayCheckId = 89;
constexpr int kEncoderPresetComboId = 90;
constexpr int kEncoderModeComboId = 91;
constexpr int kEncoderProfileComboId = 92;
constexpr int kEncoderLookaheadCheckId = 93;
constexpr int kEncoderLookaheadDepthEditId = 94;
constexpr int kEncoderSpatialAQCheckId = 95;
constexpr int kEncoderAQStrengthEditId = 96;
constexpr int kEncoderTemporalAQCheckId = 97;
constexpr int kEncoderMultipassComboId = 98;
constexpr int kEncoderBFramesCheckId = 99;
constexpr int kEncoderAdaptiveBFramesCheckId = 100;
constexpr int kBitrateEditId = 101;
constexpr int kCodecComboId = 102;
constexpr int kStartStopButtonId = 103;
constexpr int kSaveSettingsButtonId = 104;
constexpr int kClipFolderEditId = 105;
constexpr int kBrowseClipFolderButtonId = 106;
constexpr int kRecordHotkeyId = 107;
constexpr int kEncoderAdaptiveIFramesCheckId = 108;
constexpr int kEncoderZeroReorderDelayCheckId = 109;
constexpr int kEncoderGopSecondsEditId = 110;
constexpr int kEncoderReferenceFramesEditId = 111;
constexpr int kGpuAdaptiveComboId = 112;
constexpr int kGpuFrameQueueLimitEditId = 113;
constexpr int kWgcZeroCopyCheckId = 114;
constexpr int kFollowMouseMonitorCheckId = 115;
constexpr int kFollowFocusedMonitorCheckId = 116;
constexpr int kStableMultimonitorFramesCheckId = 117;
constexpr int kOutputVolumeEditId = 118;
constexpr int kInputVolumeEditId = 119;
constexpr int kSettingsCategoryButtonBaseId = 120;
constexpr int kIdleFrameCoalescingCheckId = 211;
constexpr int kReplayEnabledId = 201;
constexpr int kReplaySecondsEditId = 202;
constexpr int kSaveReplayButtonId = 203;
constexpr int kReplayHotkeyId = 204;
constexpr int kSoundSeparationAppComboId = 205;
constexpr int kSoundSeparationRefreshButtonId = 206;
constexpr int kSoundSeparationManualButtonId = 207;
constexpr int kSoundSeparationEnabledCheckId = 208;
constexpr int kRecoverFailedRecordingButtonId = 209;
constexpr int kLeagueKillReminderCheckId = 210;
constexpr int kStatsLabelId = 301;
constexpr int kCapsLabelId = 302;
constexpr int kSaveLogButtonId = 303;
constexpr int kClipListId = 401;
constexpr int kRefreshClipsButtonId = 402;
constexpr int kDeleteClipButtonId = 404;
constexpr int kRenameClipButtonId = 405;
constexpr int kFavoriteClipButtonId = 406;
constexpr int kRenameEditId = 407;
constexpr int kClipListViewButtonId = 408;
constexpr int kClipGalleryViewButtonId = 409;
constexpr int kSoundSeparationMuteButtonBaseId = 5000;
constexpr int kSoundSeparationRemoveButtonBaseId = 6000;
constexpr UINT_PTR kDiagnosticsTimerId = 1;
constexpr UINT kDiagnosticsRefreshMs = 1000;
constexpr UINT kControllerActionCompleteMessage = WM_APP + 1;
constexpr UINT kTrayMessage = WM_APP + 2;
constexpr UINT kClipThumbnailReadyMessage = WM_APP + 3;
constexpr UINT kLeagueKillDetectedMessage = WM_APP + 4;
constexpr UINT kTrayIconId = 1;
constexpr int kTrayOpenId = 9001;
constexpr int kTrayExitId = 9002;
constexpr int kTabCount = 4;
constexpr int kSettingsCategoryCount = 4;
constexpr int kWindowWidth = 900;
constexpr int kWindowHeight = 800;
constexpr int kMinWindowWidth = 620;
constexpr int kMinWindowHeight = 480;
constexpr RECT kPageRect{20, 58, 860, 700};
constexpr int kOuterMargin = 20;
constexpr int kTabTop = 14;
constexpr int kTabHeight = 32;
constexpr int kTabGap = 8;
constexpr int kPageGap = 12;
constexpr int kStatusBottomMargin = 12;
constexpr int kStatusMinHeight = 26;
constexpr int kStatusPadding = 10;
constexpr int kLayoutPadding = 24;
constexpr int kLayoutColumnGap = 28;
constexpr int kLayoutStackGap = 30;
constexpr int kTwoColumnMinimumWidth = 760;
constexpr int kComboClosedHeight = 24;
constexpr int kComboVisibleItems = 8;
constexpr COLORREF kBackground = RGB(15, 16, 18);
constexpr COLORREF kPanel = RGB(29, 31, 36);
constexpr COLORREF kText = RGB(235, 236, 240);
constexpr COLORREF kMutedText = RGB(178, 184, 194);
constexpr COLORREF kEdit = RGB(23, 25, 30);
constexpr COLORREF kSelection = RGB(58, 106, 168);
constexpr COLORREF kFavorite = RGB(255, 213, 74);
constexpr COLORREF kFavoriteText = RGB(22, 18, 6);
constexpr COLORREF kOutline = RGB(70, 76, 88);
constexpr int kListClipItemHeight = 30;
constexpr int kGalleryClipColumns = 3;
constexpr int kGalleryClipMinimumItemHeight = 220;
constexpr int kGalleryThumbnailWidth = 320;
constexpr int kGalleryThumbnailHeight = 180;

class ClipFileFormatEnumerator final : public IEnumFORMATETC {
public:
    ClipFileFormatEnumerator() = default;
    explicit ClipFileFormatEnumerator(ULONG index) : index_(index) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IEnumFORMATETC) {
            *result = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --references_;
        if (references == 0) {
            delete this;
        }
        return references;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* formats, ULONG* fetched) override {
        if (!formats || (count != 1 && !fetched)) {
            return E_POINTER;
        }
        ULONG returned = 0;
        if (index_ == 0 && count > 0) {
            formats[0] = FORMATETC{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            ++index_;
            returned = 1;
        }
        if (fetched) {
            *fetched = returned;
        }
        return returned == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        const ULONG available = 1 - index_;
        const ULONG skipped = std::min(count, available);
        index_ += skipped;
        return skipped == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        index_ = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = new ClipFileFormatEnumerator(index_);
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    ULONG index_ = 0;
};

class ClipFileDataObject final : public IDataObject {
public:
    explicit ClipFileDataObject(std::filesystem::path path) : path_(std::move(path)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
            *result = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --references_;
        if (references == 0) {
            delete this;
        }
        return references;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!format || !medium || format->cfFormat != CF_HDROP || !(format->tymed & TYMED_HGLOBAL)) {
            return DV_E_FORMATETC;
        }

        const std::wstring filename = path_.wstring();
        const SIZE_T bytes = sizeof(DROPFILES) + (filename.size() + 2) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory) {
            return E_OUTOFMEMORY;
        }

        auto* dropFiles = static_cast<DROPFILES*>(GlobalLock(memory));
        if (!dropFiles) {
            GlobalFree(memory);
            return E_OUTOFMEMORY;
        }
        dropFiles->pFiles = sizeof(DROPFILES);
        dropFiles->pt = {};
        dropFiles->fNC = FALSE;
        dropFiles->fWide = TRUE;
        auto* files = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(dropFiles) + sizeof(DROPFILES));
        std::copy(filename.begin(), filename.end(), files);
        files[filename.size()] = L'\0';
        files[filename.size() + 1] = L'\0';
        GlobalUnlock(memory);

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return DATA_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        return format && format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* result) override {
        if (result) {
            result->ptd = nullptr;
        }
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }
        *result = new ClipFileFormatEnumerator();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    std::atomic<ULONG> references_{1};
    std::filesystem::path path_;
};

class ClipFileDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
            *result = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --references_;
        if (references == 0) {
            delete this;
        }
        return references;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override {
        if (escapePressed) {
            return DRAGDROP_S_CANCEL;
        }
        return (keyState & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    std::atomic<ULONG> references_{1};
};

std::wstring readText(HWND control) {
    if (!control) {
        return {};
    }
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length + 1), L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

std::wstring trimText(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && iswspace(value.back())) {
        value.pop_back();
    }
    return value;
}

bool setText(HWND control, const std::wstring& value) {
    if (!control) {
        return false;
    }
    if (readText(control) == value) {
        return false;
    }
    SetWindowTextW(control, value.c_str());
    return true;
}

void redrawWindowAndChildren(HWND window, bool updateNow = false) {
    if (!window) {
        return;
    }
    UINT flags = RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME;
    if (updateNow) {
        flags |= RDW_UPDATENOW;
    }
    RedrawWindow(window, nullptr, nullptr, flags);
}

bool moveWindowIfChanged(HWND control, int x, int y, int width, int height) {
    if (!control) {
        return false;
    }

    RECT current{};
    if (GetWindowRect(control, &current)) {
        HWND parent = GetParent(control);
        if (parent) {
            MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&current), 2);
        }
        if (current.left == x &&
            current.top == y &&
            current.right - current.left == width &&
            current.bottom - current.top == height) {
            return false;
        }
    }

    SetWindowPos(
        control,
        nullptr,
        x,
        y,
        width,
        height,
        SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

bool showWindowIfHidden(HWND control) {
    if (!control || IsWindowVisible(control)) {
        return false;
    }
    ShowWindow(control, SW_SHOWNA);
    return true;
}

bool setListItemHeightIfChanged(HWND list, int& cachedHeight, int height) {
    if (!list || cachedHeight == height) {
        return false;
    }
    SendMessageW(list, LB_SETITEMHEIGHT, 0, height);
    cachedHeight = height;
    return true;
}

class ScopedRedrawLock {
public:
    explicit ScopedRedrawLock(HWND window)
        : window_(window) {
        if (window_) {
            SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
        }
    }

    ~ScopedRedrawLock() {
        if (window_) {
            SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
            redrawWindowAndChildren(window_);
        }
    }

    ScopedRedrawLock(const ScopedRedrawLock&) = delete;
    ScopedRedrawLock& operator=(const ScopedRedrawLock&) = delete;

private:
    HWND window_ = nullptr;
};

uint32_t readUIntControl(HWND control, uint32_t fallback) {
    const std::wstring text = trimText(readText(control));
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](wchar_t ch) { return iswdigit(ch) != 0; })) {
        return fallback;
    }
    try {
        const auto parsed = std::stoull(text);
        if (parsed > (std::numeric_limits<uint32_t>::max)()) {
            return fallback;
        }
        return static_cast<uint32_t>(parsed);
    } catch (...) {
        return fallback;
    }
}

std::wstring bytesToText(uintmax_t bytes) {
    std::wstringstream stream;
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    stream.precision(1);
    stream << std::fixed << mib << L" MiB";
    return stream.str();
}

bool isButtonId(WPARAM id) {
    const int controlId = static_cast<int>(id);
    return (controlId >= kTabButtonBaseId && controlId < kTabButtonBaseId + kTabCount) ||
           (controlId >= kSettingsCategoryButtonBaseId && controlId < kSettingsCategoryButtonBaseId + kSettingsCategoryCount) ||
           (controlId >= kSoundSeparationMuteButtonBaseId && controlId < kSoundSeparationMuteButtonBaseId + 512) ||
           (controlId >= kSoundSeparationRemoveButtonBaseId && controlId < kSoundSeparationRemoveButtonBaseId + 512) ||
           controlId == kStartStopButtonId ||
           controlId == kSaveSettingsButtonId ||
           controlId == kBrowseClipFolderButtonId ||
           controlId == kSaveReplayButtonId ||
           controlId == kRecoverFailedRecordingButtonId ||
           controlId == kSoundSeparationRefreshButtonId ||
           controlId == kSoundSeparationManualButtonId ||
           controlId == kRefreshClipsButtonId ||
           controlId == kDeleteClipButtonId ||
           controlId == kRenameClipButtonId ||
           controlId == kFavoriteClipButtonId ||
           controlId == kClipListViewButtonId ||
           controlId == kClipGalleryViewButtonId ||
           controlId == kSaveLogButtonId;
}

bool isSoundSeparationMuteButtonId(int controlId) {
    return controlId >= kSoundSeparationMuteButtonBaseId &&
           controlId < kSoundSeparationMuteButtonBaseId + 512;
}

bool isSoundSeparationRemoveButtonId(int controlId) {
    return controlId >= kSoundSeparationRemoveButtonBaseId &&
           controlId < kSoundSeparationRemoveButtonBaseId + 512;
}

size_t soundSeparationIndexFromButtonId(int controlId, int baseId) {
    return static_cast<size_t>(controlId - baseId);
}

bool isSettingsControlId(int controlId) {
    switch (controlId) {
    case kFpsEditId:
    case kResolutionModeComboId:
    case kWidthEditId:
    case kHeightEditId:
    case kSystemAudioCheckId:
    case kMicrophoneCheckId:
    case kOutputDeviceComboId:
    case kInputDeviceComboId:
    case kOutputVolumeEditId:
    case kInputVolumeEditId:
    case kStartWithWindowsCheckId:
    case kExitToTrayCheckId:
    case kEncoderPresetComboId:
    case kEncoderModeComboId:
    case kEncoderProfileComboId:
    case kEncoderLookaheadCheckId:
    case kEncoderLookaheadDepthEditId:
    case kEncoderSpatialAQCheckId:
    case kEncoderAQStrengthEditId:
    case kEncoderTemporalAQCheckId:
    case kEncoderMultipassComboId:
    case kEncoderBFramesCheckId:
    case kEncoderAdaptiveBFramesCheckId:
    case kBitrateEditId:
    case kCodecComboId:
    case kClipFolderEditId:
    case kRecordHotkeyId:
    case kEncoderAdaptiveIFramesCheckId:
    case kEncoderZeroReorderDelayCheckId:
    case kEncoderGopSecondsEditId:
    case kEncoderReferenceFramesEditId:
    case kGpuAdaptiveComboId:
    case kGpuFrameQueueLimitEditId:
    case kIdleFrameCoalescingCheckId:
    case kWgcZeroCopyCheckId:
    case kFollowMouseMonitorCheckId:
    case kFollowFocusedMonitorCheckId:
    case kStableMultimonitorFramesCheckId:
    case kReplayEnabledId:
    case kReplaySecondsEditId:
    case kReplayHotkeyId:
    case kLeagueKillReminderCheckId:
    case kSoundSeparationEnabledCheckId:
        return true;
    default:
        return false;
    }
}

std::wstring durationToText(uint64_t duration100ns) {
    if (duration100ns == 0) {
        return L"--:--";
    }

    const uint64_t totalSeconds = duration100ns / kHundredNanosecondsPerSecond;
    const uint64_t hours = totalSeconds / 3600;
    const uint64_t minutes = (totalSeconds / 60) % 60;
    const uint64_t seconds = totalSeconds % 60;

    std::wstringstream stream;
    if (hours > 0) {
        stream << hours << L":";
        if (minutes < 10) {
            stream << L"0";
        }
    }
    stream << minutes << L":";
    if (seconds < 10) {
        stream << L"0";
    }
    stream << seconds;
    return stream.str();
}

std::wstring tagsToText(const std::vector<std::wstring>& tags) {
    std::wstring value;
    for (const auto& tag : tags) {
        if (tag.empty()) {
            continue;
        }
        const std::wstring displayTag = tag == L"Automatic kill" ? L"Kill clip" : tag;
        if (!value.empty()) {
            value += L", ";
        }
        value += displayTag;
    }
    return value;
}

int comboIndexForDevice(const std::vector<AudioDeviceInfo>& devices, const std::wstring& selectedId) {
    if (selectedId.empty()) {
        return 0;
    }
    for (size_t index = 0; index < devices.size(); ++index) {
        if (devices[index].id == selectedId) {
            return static_cast<int>(index + 1);
        }
    }
    return 0;
}

std::wstring selectedDeviceId(HWND combo, const std::vector<AudioDeviceInfo>& devices) {
    const auto selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected <= 0) {
        return {};
    }
    const size_t index = static_cast<size_t>(selected - 1);
    return index < devices.size() ? devices[index].id : std::wstring();
}

bool isModifierKey(WPARAM key) {
    return key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
           key == VK_MENU || key == VK_LMENU || key == VK_RMENU ||
           key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT;
}

bool isSafeBareHotkey(uint32_t virtualKey) {
    return (virtualKey >= VK_F1 && virtualKey <= VK_F24) ||
           virtualKey == VK_PAUSE ||
           virtualKey == VK_SNAPSHOT ||
           virtualKey == VK_MEDIA_PLAY_PAUSE ||
           virtualKey == VK_MEDIA_STOP ||
           virtualKey == VK_MEDIA_NEXT_TRACK ||
           virtualKey == VK_MEDIA_PREV_TRACK;
}

uint32_t supportedHotkeyModifiers(uint32_t modifiers) {
    return modifiers & (MOD_ALT | MOD_CONTROL | MOD_SHIFT);
}

bool isSafeGlobalHotkey(uint32_t modifiers, uint32_t virtualKey) {
    if (virtualKey == 0) {
        return true;
    }
    if (virtualKey > 0xfe) {
        return false;
    }
    modifiers = supportedHotkeyModifiers(modifiers);
    if (modifiers == 0) {
        return isSafeBareHotkey(virtualKey);
    }
    if (isSafeBareHotkey(virtualKey)) {
        return true;
    }
    return (modifiers & (MOD_CONTROL | MOD_ALT)) != 0;
}

uint32_t currentModifierState() {
    uint32_t modifiers = 0;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        modifiers |= MOD_CONTROL;
    }
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
        modifiers |= MOD_ALT;
    }
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        modifiers |= MOD_SHIFT;
    }
    return modifiers;
}

bool isControlDown() {
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
}

bool shellExecuteSucceeded(HINSTANCE result) {
    return reinterpret_cast<INT_PTR>(result) > 32;
}

std::wstring keyName(uint32_t virtualKey) {
    if (virtualKey == 0) {
        return L"None";
    }

    wchar_t name[64]{};
    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    LONG lParam = static_cast<LONG>(scanCode << 16);
    if (GetKeyNameTextW(lParam, name, static_cast<int>(_countof(name))) > 0) {
        return name;
    }
    return L"VK " + std::to_wstring(virtualKey);
}

std::wstring hotkeyDisplay(uint32_t modifiers, uint32_t virtualKey) {
    if (virtualKey == 0) {
        return L"None";
    }

    std::wstring value;
    if ((modifiers & MOD_CONTROL) != 0) {
        value += L"Ctrl+";
    }
    if ((modifiers & MOD_ALT) != 0) {
        value += L"Alt+";
    }
    if ((modifiers & MOD_SHIFT) != 0) {
        value += L"Shift+";
    }
    value += keyName(virtualKey);
    return value;
}

const wchar_t* yesNo(bool value) {
    return value ? L"yes" : L"no";
}

const wchar_t* captureBackendDisplayName(CaptureBackend backend) {
    switch (backend) {
    case CaptureBackend::WindowsGraphicsCapture:
        return L"Windows Graphics Capture";
    case CaptureBackend::DesktopDuplication:
        return L"Desktop Duplication";
    }
    return L"Unknown";
}

std::wstring actionFailureStatus(const wchar_t* summary, const EncoderCapabilities& caps, const RecordingStats& stats) {
    std::wstring status(summary);
    const std::wstring encoderDetail = trimText(caps.detail);
    if (!caps.available && !encoderDetail.empty()) {
        const std::wstring backendName = caps.backendName.empty() ? L"Hardware encoder" : caps.backendName;
        return status + L": " + backendName + L" unavailable - " + encoderDetail;
    }

    const std::wstring backendStatus = trimText(stats.captureBackendStatus);
    if (!stats.captureBackendActive && !backendStatus.empty()) {
        return status + L": " + backendStatus;
    }

    return status + L"; check Diagnostics and log";
}

std::wstring withHotkeyWarning(std::wstring status, bool hotkeysOk, const std::wstring& hotkeyError) {
    if (hotkeysOk) {
        return status;
    }

    std::wstring detail = trimText(hotkeyError);
    if (detail.empty()) {
        detail = L"one or more hotkeys could not be registered";
    }
    if (!status.empty()) {
        status += L"; ";
    }
    status += L"Hotkeys not registered: " + detail;
    return status;
}

void appendFourCc(std::vector<uint8_t>& bytes, const char (&value)[5]) {
    bytes.push_back(static_cast<uint8_t>(value[0]));
    bytes.push_back(static_cast<uint8_t>(value[1]));
    bytes.push_back(static_cast<uint8_t>(value[2]));
    bytes.push_back(static_cast<uint8_t>(value[3]));
}

void appendUInt16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void appendUInt32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

std::vector<uint8_t> makeToneWav(double frequencyHz, uint32_t durationMs) {
    constexpr uint32_t kSampleRate = 44'100;
    constexpr uint16_t kChannels = 1;
    constexpr uint16_t kBitsPerSample = 16;
    constexpr uint16_t kBlockAlign = kChannels * kBitsPerSample / 8;
    constexpr uint32_t kByteRate = kSampleRate * kBlockAlign;
    constexpr double kPi = 3.14159265358979323846;

    const uint32_t sampleCount = std::max<uint32_t>(1, kSampleRate * durationMs / 1000);
    const uint32_t dataBytes = sampleCount * kBlockAlign;
    std::vector<uint8_t> bytes;
    bytes.reserve(44 + dataBytes);

    appendFourCc(bytes, "RIFF");
    appendUInt32(bytes, 36 + dataBytes);
    appendFourCc(bytes, "WAVE");
    appendFourCc(bytes, "fmt ");
    appendUInt32(bytes, 16);
    appendUInt16(bytes, 1);
    appendUInt16(bytes, kChannels);
    appendUInt32(bytes, kSampleRate);
    appendUInt32(bytes, kByteRate);
    appendUInt16(bytes, kBlockAlign);
    appendUInt16(bytes, kBitsPerSample);
    appendFourCc(bytes, "data");
    appendUInt32(bytes, dataBytes);

    const uint32_t fadeSamples = std::max<uint32_t>(1, std::min<uint32_t>(sampleCount / 2, kSampleRate / 200));
    for (uint32_t sample = 0; sample < sampleCount; ++sample) {
        double envelope = 1.0;
        if (sample < fadeSamples) {
            envelope = static_cast<double>(sample) / static_cast<double>(fadeSamples);
        } else if (sampleCount - sample < fadeSamples) {
            envelope = static_cast<double>(sampleCount - sample) / static_cast<double>(fadeSamples);
        }
        const double phase = 2.0 * kPi * frequencyHz * static_cast<double>(sample) / static_cast<double>(kSampleRate);
        const auto value = static_cast<int16_t>(std::sin(phase) * 18'000.0 * envelope);
        appendUInt16(bytes, static_cast<uint16_t>(value));
    }

    return bytes;
}

const std::vector<uint8_t>& actionIndicatorWav(UINT type) {
    static const auto attention = makeToneWav(880.0, 45);
    static const auto success = makeToneWav(1320.0, 60);
    static const auto failure = makeToneWav(220.0, 95);

    if (type == MB_ICONHAND) {
        return failure;
    }
    if (type == MB_OK) {
        return success;
    }
    return attention;
}

void playActionIndicator(UINT type) {
    const auto& wav = actionIndicatorWav(type);
    if (!PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT)) {
        MessageBeep(type);
    }
}

std::wstring thumbnailCacheKey(const std::filesystem::path& path) {
    std::wstring value = path.native();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

std::wstring executableKey(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

std::wstring appNameFromPath(const std::filesystem::path& path) {
    std::wstring name = path.stem().wstring();
    return name.empty() ? path.filename().wstring() : name;
}

HICON loadExecutableIcon(const std::filesystem::path& path) {
    SHFILEINFOW info{};
    if (!path.empty() &&
        SHGetFileInfoW(
            path.c_str(),
            0,
            &info,
            sizeof(info),
            SHGFI_ICON | SHGFI_SMALLICON) &&
        info.hIcon) {
        return info.hIcon;
    }

    HICON fallback = LoadIconW(nullptr, IDI_APPLICATION);
    return fallback ? CopyIcon(fallback) : nullptr;
}

HBITMAP loadShellThumbnail(const std::filesystem::path& path, int width, int height) {
    IShellItemImageFactory* factory = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        Logger::instance().warning(L"Could not create shell thumbnail factory for clip: " + path.wstring() + L" (" + hresultToString(hr) + L")");
        return nullptr;
    }

    SIZE size{width, height};
    HBITMAP bitmap = nullptr;
    hr = factory->GetImage(size, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bitmap);
    factory->Release();
    if (FAILED(hr) || !bitmap) {
        Logger::instance().warning(L"Could not load shell thumbnail for clip: " + path.wstring() + L" (" + hresultToString(hr) + L")");
        return nullptr;
    }
    return bitmap;
}

void drawBitmapFit(HDC dc, HBITMAP bitmap, const RECT& target) {
    if (!bitmap) {
        return;
    }

    BITMAP info{};
    if (GetObjectW(bitmap, sizeof(info), &info) != sizeof(info) || info.bmWidth <= 0 || info.bmHeight <= 0) {
        return;
    }

    const int targetWidth = std::max(1, static_cast<int>(target.right - target.left));
    const int targetHeight = std::max(1, static_cast<int>(target.bottom - target.top));
    const double scale = std::min(
        static_cast<double>(targetWidth) / static_cast<double>(info.bmWidth),
        static_cast<double>(targetHeight) / static_cast<double>(info.bmHeight));
    const int drawWidth = std::max(1, static_cast<int>(info.bmWidth * scale));
    const int drawHeight = std::max(1, static_cast<int>(info.bmHeight * scale));
    const int x = target.left + (targetWidth - drawWidth) / 2;
    const int y = target.top + (targetHeight - drawHeight) / 2;

    HDC memoryDc = CreateCompatibleDC(dc);
    if (!memoryDc) {
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
    SetStretchBltMode(dc, HALFTONE);
    StretchBlt(dc, x, y, drawWidth, drawHeight, memoryDc, 0, 0, info.bmWidth, info.bmHeight, SRCCOPY);
    SelectObject(memoryDc, oldBitmap);
    DeleteDC(memoryDc);
}

int galleryItemHeightForWidth(int listWidth) {
    constexpr int gap = 10;
    constexpr int verticalPadding = 18;
    constexpr int textHeight = 48;
    const int tileWidth = std::max(1, (listWidth - gap * (kGalleryClipColumns + 1)) / kGalleryClipColumns);
    const int previewHeight = std::max(96, tileWidth * 9 / 16);
    return std::max(kGalleryClipMinimumItemHeight, verticalPadding + previewHeight + textHeight);
}

bool resolutionPresetSize(ResolutionMode mode, uint32_t& width, uint32_t& height) {
    switch (mode) {
    case ResolutionMode::P240:
        width = 426;
        height = 240;
        return true;
    case ResolutionMode::P480:
        width = 854;
        height = 480;
        return true;
    case ResolutionMode::P720:
        width = 1280;
        height = 720;
        return true;
    case ResolutionMode::P1080:
        width = 1920;
        height = 1080;
        return true;
    case ResolutionMode::P2K:
        width = 2560;
        height = 1440;
        return true;
    case ResolutionMode::P4K:
        width = 3840;
        height = 2160;
        return true;
    case ResolutionMode::Native:
    case ResolutionMode::Custom:
        break;
    }
    return false;
}

} // namespace

MainWindow::MainWindow(RecorderController& controller, SettingsStore& settingsStore)
    : controller_(controller),
      settingsStore_(settingsStore),
      settings_(settingsStore.load()),
      clipManager_(settings_.clipDirectory),
      leagueIntegration_([this] {
          if (window_ && IsWindow(window_)) {
              PostMessageW(window_, kLeagueKillDetectedMessage, 0, 0);
          }
      }) {
    libraryViewMode_ = settings_.libraryGalleryView ? LibraryViewMode::Gallery : LibraryViewMode::List;
    recordHotkeyModifiers_ = settings_.hotkeys.startStopModifiers;
    recordHotkeyVirtualKey_ = settings_.hotkeys.startStopVirtualKey;
    replayHotkeyModifiers_ = settings_.hotkeys.saveReplayModifiers;
    replayHotkeyVirtualKey_ = settings_.hotkeys.saveReplayVirtualKey;
}

MainWindow::~MainWindow() {
    stopGameIntegrations();
    stopThumbnailWorker();
    stopControllerWorker();
    releaseClipThumbnailCache();
    if (backgroundBrush_) {
        DeleteObject(backgroundBrush_);
    }
    if (headingFont_) {
        DeleteObject(headingFont_);
    }
    if (controlBrush_) {
        DeleteObject(controlBrush_);
    }
    if (editBrush_) {
        DeleteObject(editBrush_);
    }
    if (selectionBrush_) {
        DeleteObject(selectionBrush_);
    }
    if (favoriteBrush_) {
        DeleteObject(favoriteBrush_);
    }
    if (outlinePen_) {
        DeleteObject(outlinePen_);
    }
    if (selectedOutlinePen_) {
        DeleteObject(selectedOutlinePen_);
    }
}

bool MainWindow::create(HINSTANCE instance, int showCommand, bool startMinimized) {
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_TAB_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&commonControls);

    font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    LOGFONTW headingFont{};
    if (GetObjectW(font_, sizeof(headingFont), &headingFont) == sizeof(headingFont)) {
        headingFont.lfWeight = FW_SEMIBOLD;
        headingFont_ = CreateFontIndirectW(&headingFont);
    }
    backgroundBrush_ = CreateSolidBrush(kBackground);
    controlBrush_ = CreateSolidBrush(kPanel);
    editBrush_ = CreateSolidBrush(kEdit);
    selectionBrush_ = CreateSolidBrush(kSelection);
    favoriteBrush_ = CreateSolidBrush(kFavorite);
    outlinePen_ = CreatePen(PS_SOLID, 1, kOutline);
    selectedOutlinePen_ = CreatePen(PS_SOLID, 1, kText);

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainWindow::windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!wc.hIcon) {
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    wc.hbrBackground = backgroundBrush_;
    wc.lpszClassName = kBacktrackMainWindowClassName;
    RegisterClassW(&wc);

    WNDCLASSW pageHostClass{};
    pageHostClass.lpfnWndProc = MainWindow::pageHostProc;
    pageHostClass.hInstance = instance;
    pageHostClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pageHostClass.hbrBackground = controlBrush_;
    pageHostClass.lpszClassName = L"BacktrackPageHost";
    RegisterClassW(&pageHostClass);

    window_ = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Backtrack",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kWindowWidth,
        kWindowHeight,
        nullptr,
        nullptr,
        instance,
        this);

    if (!window_) {
        return false;
    }
    applyDarkTheme(window_);

    pageHost_ = CreateWindowExW(
        0,
        pageHostClass.lpszClassName,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
        0,
        0,
        0,
        0,
        window_,
        nullptr,
        instance,
        this);
    if (!pageHost_) {
        return false;
    }
    applyDarkTheme(pageHost_);

    buildTabs();
    status_ = CreateWindowExW(
        0,
        L"STATIC",
        statusText_.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | WS_BORDER,
        0,
        0,
        0,
        0,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusId)),
        instance,
        nullptr);
    setDefaultFont(status_);
    applyDarkTheme(status_);
    displayedStatusText_ = statusText_;
    switchPage(Page::Capture);
    layoutWindow();

    startControllerWorker();
    const bool hotkeysOk = hotkeys_.registerHotkeys(window_, settings_.hotkeys);
    const std::wstring hotkeyError = hotkeys_.lastErrorMessage();
    int initialShowCommand = showCommand;
    if (initialShowCommand == SW_HIDE ||
        initialShowCommand == SW_SHOWDEFAULT ||
        initialShowCommand == SW_SHOWMINIMIZED ||
        initialShowCommand == SW_SHOWMINNOACTIVE ||
        initialShowCommand == SW_MINIMIZE) {
        initialShowCommand = SW_SHOWNORMAL;
    }
    ShowWindow(window_, startMinimized ? SW_SHOWMINIMIZED : initialShowCommand);
    UpdateWindow(window_);
    if (settings_.replay.enabled) {
        setStatus(L"Starting replay buffer...");
    }
    ControllerAction action;
    action.kind = ControllerActionKind::Startup;
    action.settings = settings_;
    action.hotkeysOk = hotkeysOk;
    action.hotkeyError = hotkeyError;
    queueControllerAction(std::move(action), L"Recorder is starting");
    return true;
}

int MainWindow::runMessageLoop() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (handleShortcut(message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void MainWindow::startControllerWorker() {
    if (controllerWorker_.joinable()) {
        return;
    }
    {
        std::scoped_lock lock(controllerQueueMutex_);
        controllerWorkerStopping_ = false;
        controllerQueue_.clear();
    }
    controllerWorker_ = std::thread(&MainWindow::controllerWorkerLoop, this);
}

void MainWindow::stopControllerWorker() {
    {
        std::scoped_lock lock(controllerQueueMutex_);
        controllerWorkerStopping_ = true;
        controllerQueue_.clear();
        controllerActionPending_ = false;
    }
    controllerQueueCv_.notify_all();
    if (controllerWorker_.joinable()) {
        controllerWorker_.join();
    }
}

bool MainWindow::queueControllerAction(ControllerAction action, const std::wstring& busyStatus) {
    if (controllerActionPending_.exchange(true)) {
        setStatus(busyStatus);
        playActionIndicator(MB_ICONHAND);
        return false;
    }

    {
        std::scoped_lock lock(controllerQueueMutex_);
        if (controllerWorkerStopping_) {
            controllerActionPending_ = false;
            return false;
        }
        controllerQueue_.push_back(std::move(action));
    }
    controllerQueueCv_.notify_one();
    return true;
}

void MainWindow::controllerWorkerLoop() {
    setThreadDescriptionSafe(L"Backtrack controller");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    for (;;) {
        ControllerAction action;
        {
            std::unique_lock lock(controllerQueueMutex_);
            controllerQueueCv_.wait(lock, [&] {
                return controllerWorkerStopping_ || !controllerQueue_.empty();
            });
            if (controllerWorkerStopping_ && controllerQueue_.empty()) {
                break;
            }
            action = std::move(controllerQueue_.front());
            controllerQueue_.pop_front();
        }

        auto result = std::make_unique<ControllerActionResult>(executeControllerAction(action));

        bool stopping = false;
        {
            std::scoped_lock lock(controllerQueueMutex_);
            stopping = controllerWorkerStopping_;
        }
        if (!stopping && window_ && IsWindow(window_)) {
            auto* rawResult = result.release();
            if (!PostMessageW(window_, kControllerActionCompleteMessage, 0, reinterpret_cast<LPARAM>(rawResult))) {
                delete rawResult;
                controllerActionPending_ = false;
            }
        } else {
            controllerActionPending_ = false;
        }
    }

    controller_.shutdown();
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
}

MainWindow::ControllerActionResult MainWindow::executeControllerAction(const ControllerAction& action) {
    ControllerActionResult result;
    result.kind = action.kind;

    switch (action.kind) {
    case ControllerActionKind::Startup:
        result.ok = controller_.updateSettings(action.settings);
        result.status = result.ok
            ? L"Ready"
            : actionFailureStatus(L"Capture could not start", controller_.encoderCapabilities(), controller_.stats());
        result.status = withHotkeyWarning(std::move(result.status), action.hotkeysOk, action.hotkeyError);
        break;
    case ControllerActionKind::ApplySettings:
        result.ok = controller_.updateSettings(action.settings);
        result.status = result.ok
            ? L"Settings saved"
            : actionFailureStatus(L"Settings saved, but capture could not restart", controller_.encoderCapabilities(), controller_.stats());
        result.status = withHotkeyWarning(std::move(result.status), action.hotkeysOk, action.hotkeyError);
        break;
    case ControllerActionKind::ToggleRecording:
        playActionIndicator(MB_ICONASTERISK);
        if (controller_.stats().recording) {
            result.clipPath = controller_.stopRecording();
            result.stoppedRecording = true;
            result.ok = !result.clipPath.empty();
            result.refreshLibrary = result.ok;
            playActionIndicator(result.ok ? MB_OK : MB_ICONHAND);
            const std::wstring detail = trimText(controller_.lastRecordingError());
            result.status = result.ok
                ? L"Saved " + result.clipPath.filename().wstring()
                : (!detail.empty() ? detail : L"No video frames captured; clip was not created");
        } else {
            result.ok = controller_.startRecording();
            result.startedRecording = result.ok;
            playActionIndicator(result.ok ? MB_OK : MB_ICONHAND);
            result.status = result.ok
                ? L"Recording started"
                : actionFailureStatus(L"Recording could not start", controller_.encoderCapabilities(), controller_.stats());
        }
        break;
    case ControllerActionKind::SaveReplay:
        playActionIndicator(MB_ICONASTERISK);
        result.clipPath = controller_.saveReplay();
        result.savedReplay = !result.clipPath.empty();
        if (result.savedReplay && !action.replayTag.empty()) {
            ClipManager manager(controller_.settings().clipDirectory);
            if (!manager.addTag(result.clipPath, action.replayTag)) {
                Logger::instance().warning(L"Could not tag saved replay: " + result.clipPath.wstring());
            }
        }
        result.ok = result.savedReplay;
        result.refreshLibrary = result.savedReplay;
        playActionIndicator(result.savedReplay ? MB_OK : MB_ICONHAND);
        if (result.savedReplay) {
            result.status = action.replayTag.empty()
                ? L"Replay saved"
                : action.replayTag + L" saved";
        } else {
            const std::wstring failureSummary = action.replayTag.empty()
                ? L"Replay could not be saved"
                : action.replayTag + L" could not be saved";
            result.status = actionFailureStatus(
                failureSummary.c_str(),
                controller_.encoderCapabilities(),
                controller_.stats());
        }
        break;
    case ControllerActionKind::RecoverFailedRecording:
        playActionIndicator(MB_ICONASTERISK);
        result.clipPath = controller_.recoverFailedRecording();
        result.recoveredRecording = !result.clipPath.empty();
        result.ok = result.recoveredRecording;
        result.refreshLibrary = result.recoveredRecording;
        playActionIndicator(result.recoveredRecording ? MB_OK : MB_ICONHAND);
        {
            const std::wstring detail = trimText(controller_.lastRecordingError());
            result.status = result.recoveredRecording
                ? L"Recovered " + result.clipPath.filename().wstring()
                : (!detail.empty() ? detail : L"No failed recording recovery data was found");
        }
        break;
    }

    return result;
}

void MainWindow::handleControllerActionComplete(const ControllerActionResult& result) {
    controllerActionPending_ = false;
    if (result.kind == ControllerActionKind::Startup || result.kind == ControllerActionKind::ApplySettings) {
        updateGameIntegrations();
    }
    if (!result.status.empty()) {
        setStatus(result.status);
    }
    if (result.kind == ControllerActionKind::ToggleRecording && page_ == Page::Capture) {
        HWND button = startStopButton_ ? startStopButton_ : GetDlgItem(pageHost_, kStartStopButtonId);
        setText(button, controller_.stats().recording ? L"Stop Recording" : L"Start Recording");
        InvalidateRect(button, nullptr, FALSE);
    }
    if (result.refreshLibrary && page_ == Page::Library) {
        refreshClips();
    }
}

void MainWindow::updateDiagnosticsTimer() {
    if (!window_) {
        return;
    }

    const bool shouldRun =
        page_ == Page::Diagnostics &&
        IsWindowVisible(window_) &&
        !IsIconic(window_);
    if (shouldRun) {
        if (!diagnosticsTimerActive_) {
            diagnosticsTimerActive_ = SetTimer(window_, kDiagnosticsTimerId, kDiagnosticsRefreshMs, nullptr) != 0;
        }
        return;
    }

    if (diagnosticsTimerActive_) {
        KillTimer(window_, kDiagnosticsTimerId);
        diagnosticsTimerActive_ = false;
    }
}

LRESULT CALLBACK MainWindow::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    }

    if (self) {
        return self->handleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::pageHostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if (!self) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_MEASUREITEM:
    case WM_DRAWITEM:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return SendMessageW(self->window_, message, wParam, lParam);
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            self->updateStatusHelp(reinterpret_cast<HWND>(wParam));
        }
        break;
    case WM_MOUSEMOVE:
        self->updateStatusHelp(nullptr);
        break;
    case WM_MOUSEWHEEL:
        self->scrollPageWheel(wParam);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_ALL;
        GetScrollInfo(window, SB_VERT, &info);

        int position = info.nPos;
        switch (LOWORD(wParam)) {
        case SB_LINEUP:
            position -= 24;
            break;
        case SB_LINEDOWN:
            position += 24;
            break;
        case SB_PAGEUP:
            position -= static_cast<int>(info.nPage);
            break;
        case SB_PAGEDOWN:
            position += static_cast<int>(info.nPage);
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            position = info.nTrackPos;
            break;
        default:
            break;
        }
        self->scrollPageTo(position);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(dc, &rect, self->controlBrush_);

        HGDIOBJ oldPen = SelectObject(dc, self->outlinePen_);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);

        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return TRUE;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::clipListSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData) {
    auto* self = reinterpret_cast<MainWindow*>(refData);
    switch (message) {
    case WM_LBUTTONDOWN:
        if (self) {
            const POINT point{
                static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
                static_cast<LONG>(static_cast<short>(HIWORD(lParam))),
            };
            self->armClipDrag(point);
            const LRESULT defaultResult = DefSubclassProc(window, message, wParam, lParam);
            if (self->clipDragArmed_ && DragDetect(window, point)) {
                self->beginClipDrag();
            }
            self->clipDragArmed_ = false;
            self->clipDragPath_.clear();
            return defaultResult;
        }
        break;
    case WM_LBUTTONUP:
        if (self) {
            self->clipDragArmed_ = false;
            self->clipDragPath_.clear();
        }
        break;
    case WM_MOUSEWHEEL:
        if (self && self->handleClipListMouseWheel(wParam)) {
            return 0;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, clipListSubclassProc, subclassId);
        break;
    default:
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT MainWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == backtrackActivationMessage()) {
        restoreFromTray();
        return 0;
    }

    switch (message) {
    case WM_COMMAND: {
        const int controlId = static_cast<int>(LOWORD(wParam));
        const int notification = static_cast<int>(HIWORD(wParam));
        if (controlId == kResolutionModeComboId && notification == CBN_SELCHANGE) {
            updateResolutionControls();
            markSettingsDirty();
            return 0;
        }
        if (controlId == kFollowFocusedMonitorCheckId && notification == BN_CLICKED) {
            updateResolutionControls();
            markSettingsDirty();
            return 0;
        }
        if (controlId == kSoundSeparationAppComboId && notification == CBN_SELCHANGE) {
            addSelectedSoundSeparationApp();
            return 0;
        }
        if (isSoundSeparationMuteButtonId(controlId) && notification == BN_CLICKED) {
            toggleSoundSeparationApp(soundSeparationIndexFromButtonId(controlId, kSoundSeparationMuteButtonBaseId));
            return 0;
        }
        if (isSoundSeparationRemoveButtonId(controlId) && notification == BN_CLICKED) {
            removeSoundSeparationApp(soundSeparationIndexFromButtonId(controlId, kSoundSeparationRemoveButtonBaseId));
            return 0;
        }
        if (isSettingsControlId(controlId) &&
            (notification == EN_CHANGE || notification == CBN_SELCHANGE || notification == BN_CLICKED)) {
            markSettingsDirty();
        }
        switch (controlId) {
        case kTabButtonBaseId:
        case kTabButtonBaseId + 1:
        case kTabButtonBaseId + 2:
        case kTabButtonBaseId + 3:
            switchPage(static_cast<Page>(controlId - kTabButtonBaseId));
            return 0;
        case kSettingsCategoryButtonBaseId:
        case kSettingsCategoryButtonBaseId + 1:
        case kSettingsCategoryButtonBaseId + 2:
        case kSettingsCategoryButtonBaseId + 3:
            switchSettingsCategory(static_cast<SettingsCategory>(controlId - kSettingsCategoryButtonBaseId));
            return 0;
        case kStartStopButtonId:
            handleHotkey(HotkeyService::kStartStopId);
            return 0;
        case kSaveSettingsButtonId:
            applyVisibleSettings();
            return 0;
        case kBrowseClipFolderButtonId:
            browseClipFolder();
            return 0;
        case kSoundSeparationRefreshButtonId:
            refreshSoundSeparationApps();
            return 0;
        case kSoundSeparationManualButtonId:
            addManualSoundSeparationApp();
            return 0;
        case kSaveReplayButtonId:
            handleHotkey(HotkeyService::kSaveReplayId);
            return 0;
        case kSaveLogButtonId:
            saveLog();
            return 0;
        case kRecoverFailedRecordingButtonId: {
            ControllerAction action;
            action.kind = ControllerActionKind::RecoverFailedRecording;
            if (queueControllerAction(std::move(action), L"Recorder is busy; recovery ignored")) {
                setStatus(L"Recovering failed recording...");
            }
            return 0;
        }
        case kRefreshClipsButtonId:
            refreshClips();
            return 0;
        case kDeleteClipButtonId:
            deleteSelectedClip();
            return 0;
        case kRenameClipButtonId:
            renameSelectedClip();
            return 0;
        case kFavoriteClipButtonId:
            toggleSelectedFavorite();
            return 0;
        case kClipListViewButtonId:
            setLibraryViewMode(LibraryViewMode::List);
            return 0;
        case kClipGalleryViewButtonId:
            setLibraryViewMode(LibraryViewMode::Gallery);
            return 0;
        case kClipListId:
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                updateGallerySelectionFromCursor();
                onClipSelectionChanged();
            } else if (HIWORD(wParam) == LBN_DBLCLK) {
                updateGallerySelectionFromCursor();
                openSelectedClip();
            }
            return 0;
        }
        break;
    }
    case kTrayMessage:
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            restoreFromTray();
            return 0;
        }
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            showTrayMenu();
            return 0;
        }
        break;
    case WM_HOTKEY:
        if (hotkeys_.shouldHandleHotkeyMessage(static_cast<int>(wParam), lParam)) {
            handleHotkey(static_cast<int>(wParam));
        }
        return 0;
    case kControllerActionCompleteMessage: {
        std::unique_ptr<ControllerActionResult> result(reinterpret_cast<ControllerActionResult*>(lParam));
        if (result) {
            handleControllerActionComplete(*result);
        }
        return 0;
    }
    case kLeagueKillDetectedMessage:
        handleLeagueKillDetected();
        return 0;
    case kClipThumbnailReadyMessage: {
        std::unique_ptr<ThumbnailResult> result(reinterpret_cast<ThumbnailResult*>(lParam));
        if (result) {
            handleClipThumbnailReady(*result);
        }
        return 0;
    }
    case WM_NOTIFY:
        break;
    case WM_DPICHANGED: {
        if (lParam) {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(
                window_,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        layoutWindow();
        redrawWindowAndChildren(window_, true);
        return 0;
    }
    case WM_DISPLAYCHANGE:
        layoutWindow();
        redrawWindowAndChildren(window_, true);
        return 0;
    case WM_SIZE:
        layoutWindow();
        updateDiagnosticsTimer();
        return 0;
    case WM_SHOWWINDOW:
        updateDiagnosticsTimer();
        break;
    case WM_GETMINMAXINFO: {
        auto* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
        minMax->ptMinTrackSize.x = kMinWindowWidth;
        minMax->ptMinTrackSize.y = kMinWindowHeight;
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            updateStatusHelp(reinterpret_cast<HWND>(wParam));
        }
        break;
    case WM_MOUSEMOVE:
        updateStatusHelp(nullptr);
        break;
    case WM_TIMER:
        if (wParam == kDiagnosticsTimerId) {
            if (page_ == Page::Diagnostics && IsWindowVisible(window_) && !IsIconic(window_)) {
                updateStats();
            } else {
                updateDiagnosticsTimer();
            }
            return 0;
        }
        break;
    case WM_MEASUREITEM:
        if (wParam == kClipListId) {
            reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight =
                clipListItemHeight_ > 0 ? clipListItemHeight_ : (libraryViewMode_ == LibraryViewMode::Gallery ? kGalleryClipMinimumItemHeight : kListClipItemHeight);
            return TRUE;
        }
        break;
    case WM_DRAWITEM:
        if (wParam == kClipListId) {
            drawClipItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        if (isButtonId(wParam)) {
            drawButtonItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC:
        SetTextColor(reinterpret_cast<HDC>(wParam), isSoundSeparationMutedLabel(reinterpret_cast<HWND>(lParam)) ? RGB(232, 72, 72) : kText);
        if (reinterpret_cast<HWND>(lParam) == status_) {
            SetBkMode(reinterpret_cast<HDC>(wParam), OPAQUE);
            SetBkColor(reinterpret_cast<HDC>(wParam), kPanel);
            return reinterpret_cast<LRESULT>(controlBrush_);
        }
        SetBkMode(reinterpret_cast<HDC>(wParam), OPAQUE);
        SetBkColor(reinterpret_cast<HDC>(wParam), kPanel);
        return reinterpret_cast<LRESULT>(controlBrush_);
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wParam), kText);
        SetBkColor(reinterpret_cast<HDC>(wParam), kEdit);
        return reinterpret_cast<LRESULT>(editBrush_);
    case WM_CTLCOLORBTN:
        SetTextColor(reinterpret_cast<HDC>(wParam), kText);
        SetBkColor(reinterpret_cast<HDC>(wParam), kPanel);
        return reinterpret_cast<LRESULT>(controlBrush_);
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window_, &paint);
        RECT rect{};
        GetClientRect(window_, &rect);
        FillRect(dc, &rect, backgroundBrush_);

        EndPaint(window_, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return TRUE;
    case WM_CLOSE:
        if (settings_.exitToTray && !exitFromTray_) {
            addTrayIcon();
            ShowWindow(window_, SW_HIDE);
            setStatus(L"Backtrack is running in the tray");
            return 0;
        }
        break;
    case WM_DESTROY:
        KillTimer(window_, kDiagnosticsTimerId);
        diagnosticsTimerActive_ = false;
        removeTrayIcon();
        hotkeys_.unregisterHotkeys(window_);
        stopGameIntegrations();
        stopThumbnailWorker();
        releaseClipThumbnailCache();
        stopControllerWorker();
        controller_.shutdown();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window_, message, wParam, lParam);
}

void MainWindow::buildTabs() {
    const wchar_t* labels[] = {L"Capture", L"Library", L"Settings", L"Diagnostics"};
    for (int index = 0; index < kTabCount; ++index) {
        HWND button = CreateWindowExW(
            0,
            L"BUTTON",
            labels[index],
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            0,
            0,
            0,
            0,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabButtonBaseId + index)),
            nullptr,
            nullptr);
        setDefaultFont(button);
        applyDarkTheme(button);
        tabButtons_.push_back(button);
    }
}

void MainWindow::switchPage(Page page) {
    page_ = page;
    settingsDirty_ = false;
    pageScrollY_ = 0;
    pageWheelRemainder_ = 0;
    clipListWheelRemainder_ = 0;
    if (pageHost_) {
        SendMessageW(pageHost_, WM_SETREDRAW, FALSE, 0);
    }
    clearPageControls();
    for (size_t index = 0; index < tabButtons_.size(); ++index) {
        EnableWindow(tabButtons_[index], index != static_cast<size_t>(page_));
    }
    buildingPage_ = true;
    switch (page_) {
    case Page::Capture:
        buildCapturePage();
        break;
    case Page::Library:
        buildClipsPage();
        break;
    case Page::Settings:
        buildSettingsPage();
        break;
    case Page::Diagnostics:
        buildStatsPage();
        break;
    }
    buildingPage_ = false;
    clearSettingsDirty();
    applyStatusText(statusText_);
    layoutCurrentPage();
    if (pageHost_) {
        SendMessageW(pageHost_, WM_SETREDRAW, TRUE, 0);
        redrawWindowAndChildren(pageHost_, true);
    }
    redrawWindowAndChildren(window_);
    updateDiagnosticsTimer();
}

void MainWindow::switchSettingsCategory(SettingsCategory category) {
    settingsCategory_ = category;
    switchPage(Page::Settings);
}

void MainWindow::clearPageControls() {
    for (HWND control : pageControls_) {
        DestroyWindow(control);
    }
    releaseSoundSeparationRowIcons();
    pageControls_.clear();
    layoutItems_.clear();
    statusHelpTexts_.clear();
    hoveredHelpControl_ = nullptr;
    applyStatusText(statusText_);
    saveSettingsButton_ = nullptr;
    startStopButton_ = nullptr;
    bitrateEdit_ = nullptr;
    codecCombo_ = nullptr;
    recordHotkey_ = nullptr;
    clipFolderEdit_ = nullptr;
    replayEnabledCheck_ = nullptr;
    replaySecondsEdit_ = nullptr;
    replayHotkey_ = nullptr;
    leagueKillReminderCheck_ = nullptr;
    statsLabel_ = nullptr;
    capsLabel_ = nullptr;
    diagnosticLogLabel_ = nullptr;
    clipList_ = nullptr;
    listViewButton_ = nullptr;
    galleryViewButton_ = nullptr;
    settingsCategoryButtons_.clear();
    renameEdit_ = nullptr;
    selectedClipIndex_ = static_cast<size_t>(-1);
    clearClipThumbnails();
    fpsEdit_ = nullptr;
    resolutionModeCombo_ = nullptr;
    widthEdit_ = nullptr;
    heightEdit_ = nullptr;
    followFocusedMonitorCheck_ = nullptr;
    followMouseMonitorCheck_ = nullptr;
    systemAudioCheck_ = nullptr;
    microphoneCheck_ = nullptr;
    outputDeviceCombo_ = nullptr;
    inputDeviceCombo_ = nullptr;
    outputVolumeEdit_ = nullptr;
    inputVolumeEdit_ = nullptr;
    startWithWindowsCheck_ = nullptr;
    exitToTrayCheck_ = nullptr;
    encoderPresetCombo_ = nullptr;
    encoderModeCombo_ = nullptr;
    encoderProfileCombo_ = nullptr;
    encoderLookaheadCheck_ = nullptr;
    encoderLookaheadDepthEdit_ = nullptr;
    encoderSpatialAQCheck_ = nullptr;
    encoderAQStrengthEdit_ = nullptr;
    encoderTemporalAQCheck_ = nullptr;
    encoderMultipassCombo_ = nullptr;
    encoderBFramesCheck_ = nullptr;
    encoderAdaptiveBFramesCheck_ = nullptr;
    encoderAdaptiveIFramesCheck_ = nullptr;
    encoderZeroReorderDelayCheck_ = nullptr;
    encoderGopSecondsEdit_ = nullptr;
    encoderReferenceFramesEdit_ = nullptr;
    gpuAdaptiveCombo_ = nullptr;
    gpuFrameQueueLimitEdit_ = nullptr;
    idleFrameCoalescingCheck_ = nullptr;
    wgcZeroCopyCheck_ = nullptr;
    stableMultimonitorFramesCheck_ = nullptr;
    soundSeparationEnabledCheck_ = nullptr;
    soundSeparationAppCombo_ = nullptr;
    soundSeparationRefreshButton_ = nullptr;
    soundSeparationManualButton_ = nullptr;
    soundSeparationAvailableApps_.clear();
    soundSeparationRowsY_ = 0;
    clipListItemHeight_ = 0;
}

HWND MainWindow::addControl(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id) {
    DWORD finalStyle = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style;
    if (lstrcmpW(className, L"BUTTON") == 0 && (style & BS_AUTOCHECKBOX) == 0) {
        finalStyle &= ~BS_PUSHBUTTON;
        finalStyle |= BS_OWNERDRAW;
    }
    const bool comboBox = lstrcmpW(className, WC_COMBOBOXW) == 0;
    const bool staticText = lstrcmpW(className, L"STATIC") == 0;
    const int layoutHeight = comboBox ? kComboClosedHeight : height;
    const int windowHeight = comboBox ? height : layoutHeight;

    RECT design{
        x - kPageRect.left,
        y - kPageRect.top,
        x - kPageRect.left + width,
        y - kPageRect.top + layoutHeight,
    };

    HWND control = CreateWindowExW(
        0,
        className,
        text,
        finalStyle,
        design.left,
        design.top,
        width,
        windowHeight,
        pageHost_ ? pageHost_ : window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        nullptr,
        nullptr);
    setDefaultFont(control);
    if (!staticText) {
        applyDarkTheme(control);
    }
    if (comboBox) {
        SendMessageW(control, CB_SETMINVISIBLE, kComboVisibleItems, 0);
    }
    pageControls_.push_back(control);
    layoutItems_.push_back(LayoutItem{control, design, windowHeight, LayoutItem::Kind::Content});
    SetWindowPos(control, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return control;
}

void MainWindow::setDefaultFont(HWND control) {
    if (control && font_) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
}

HWND MainWindow::addSectionLabel(const wchar_t* text, int x, int y, int width) {
    HWND label = addControl(L"STATIC", text, SS_LEFT, x, y, width, 22, -1);
    if (label && headingFont_) {
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);
    }
    return label;
}

void MainWindow::buildSettingsPage() {
    settings_ = controller_.settings();
    buildSettingsCategoryTabs(78);
    if (settingsCategory_ == SettingsCategory::General) {
        buildSettingsGeneralPage();
    } else if (settingsCategory_ == SettingsCategory::Advanced) {
        buildSettingsAdvancedPage();
    } else if (settingsCategory_ == SettingsCategory::SoundSeparation) {
        buildSettingsSoundSeparationPage();
    } else {
        buildSettingsGameIntegrationsPage();
    }
    addDirtySaveButton();
}

void MainWindow::buildSettingsCategoryTabs(int y) {
    constexpr int kTabX = 44;
    constexpr int kTabWidth = 150;
    constexpr int kTabHeight = 30;
    constexpr int kTabGap = 8;
    const wchar_t* labels[] = {L"General", L"Advanced", L"Sound seperation", L"Game integrations"};

    settingsCategoryButtons_.clear();
    for (int index = 0; index < kSettingsCategoryCount; ++index) {
        HWND button = addControl(
            L"BUTTON",
            labels[index],
            BS_PUSHBUTTON | WS_TABSTOP,
            kTabX + index * (kTabWidth + kTabGap),
            y,
            kTabWidth,
            kTabHeight,
            kSettingsCategoryButtonBaseId + index);
        settingsCategoryButtons_.push_back(button);
        EnableWindow(button, index != static_cast<int>(settingsCategory_));
    }
}

void MainWindow::buildSettingsGeneralPage() {
    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kControlWidth = 360;
    constexpr int kWideControlWidth = 430;
    constexpr int kNumericWidth = 96;
    constexpr int kRowHeight = 38;
    constexpr int kSectionGap = 18;
    int y = 128;

    auto addRowLabel = [&](const wchar_t* text) {
        addControl(L"STATIC", text, 0, kX, y + 4, kLabelWidth, 22, -1);
    };
    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 36;
    };
    auto finishSection = [&]() {
        y += kSectionGap;
    };

    addSection(L"App");
    addRowLabel(L"Startup");
    startWithWindowsCheck_ = addControl(L"BUTTON", L"Start minimized with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kStartWithWindowsCheckId);
    SendMessageW(startWithWindowsCheck_, BM_SETCHECK, settings_.startWithWindowsMinimized ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(startWithWindowsCheck_, 0, 0, L"Adds Backtrack to the current user startup list and starts it minimized after Windows sign-in.");
    y += kRowHeight;

    addRowLabel(L"Close button");
    exitToTrayCheck_ = addControl(L"BUTTON", L"Exit into tray", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kExitToTrayCheckId);
    SendMessageW(exitToTrayCheck_, BM_SETCHECK, settings_.exitToTray ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(exitToTrayCheck_, 0, 0, L"Closing the window hides Backtrack in the tray instead of shutting down recording and replay services.");
    y += kRowHeight;
    finishSection();

    addSection(L"Output");
    addRowLabel(L"Codec");
    codecCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 120, kCodecComboId);
    SendMessageW(codecCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"H.264"));
    SendMessageW(codecCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"HEVC"));
    SendMessageW(codecCombo_, CB_SETCURSEL, settings_.video.codec == VideoCodec::H264 ? 0 : 1, 0);
    addSettingHelp(codecCombo_, 0, 0, L"Video codec for hardware encoder output. H.264 has broad compatibility; HEVC can improve quality per bit but may be less compatible.");
    y += kRowHeight;

    addRowLabel(L"Bitrate (Kbps)");
    bitrateEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.bitrateKbps).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kBitrateEditId);
    addSettingHelp(bitrateEdit_, 0, 0, L"Target video bitrate in kilobits per second. Higher values preserve detail but increase file size and encoder bandwidth.");
    y += kRowHeight;

    addRowLabel(L"Frame rate");
    fpsEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.fps).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kFpsEditId);
    addSettingHelp(fpsEdit_, 0, 0, L"Target recording frame rate. Higher FPS captures smoother motion but increases capture, encode, and replay-buffer work.");
    y += kRowHeight;

    addRowLabel(L"Resolution");
    resolutionModeCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 220, kResolutionModeComboId);
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Native display"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"240p"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"480p"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"720p"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1080p"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2K"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"4K"));
    SendMessageW(resolutionModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom size"));
    SendMessageW(resolutionModeCombo_, CB_SETCURSEL, static_cast<WPARAM>(settings_.video.resolutionMode), 0);
    addSettingHelp(resolutionModeCombo_, 0, 0, L"Native records at the captured display size. Presets scale to fixed 16:9 encode sizes. Custom enables Width and Height and may use a D3D11 scaling pass.");
    y += kRowHeight;

    addRowLabel(L"Custom size");
    addControl(L"STATIC", L"Width", 0, kControlX, y + 4, 48, 22, -1);
    widthEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.width).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX + 56, y, kNumericWidth, 24, kWidthEditId);
    addSettingHelp(widthEdit_, 0, 0, L"Custom encode width. Lower width reduces GPU encode load and bitrate demand but softens the image.");
    addControl(L"STATIC", L"Height", 0, kControlX + 190, y + 4, 56, 22, -1);
    heightEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.height).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX + 254, y, kNumericWidth, 24, kHeightEditId);
    addSettingHelp(heightEdit_, 0, 0, L"Custom encode height. Lower height reduces GPU encode load and bitrate demand but softens the image.");
    y += kRowHeight;

    addRowLabel(L"Monitor");
    followFocusedMonitorCheck_ = addControl(L"BUTTON", L"Follow focused monitor", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kFollowFocusedMonitorCheckId);
    SendMessageW(followFocusedMonitorCheck_, BM_SETCHECK, settings_.followFocusedMonitor ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(followFocusedMonitorCheck_, 0, 0, L"Enables monitor switching during one continuous recording or replay buffer. By default Backtrack records the monitor containing the foreground window and uses Windows Graphics Capture.");
    y += kRowHeight;

    followMouseMonitorCheck_ = addControl(L"BUTTON", L"Follow mouse", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kFollowMouseMonitorCheckId);
    SendMessageW(followMouseMonitorCheck_, BM_SETCHECK, settings_.followMouseMonitor ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(followMouseMonitorCheck_, 0, 0, L"Uses the mouse cursor instead of the foreground window to choose the recorded monitor when monitor following is enabled.");
    y += kRowHeight;

    addRowLabel(L"Clip folder");
    clipFolderEdit_ = addControl(L"EDIT", settings_.clipDirectory.wstring().c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kControlX, y, kWideControlWidth, 24, kClipFolderEditId);
    addSettingHelp(clipFolderEdit_, 0, 0, L"Folder where recordings and saved replay clips are written.");
    addControl(L"BUTTON", L"Browse", BS_PUSHBUTTON | WS_TABSTOP, kControlX + kWideControlWidth + 12, y - 2, 88, 30, kBrowseClipFolderButtonId);
    y += kRowHeight;
    updateResolutionControls();
    finishSection();

    addSection(L"Audio");
    addRowLabel(L"Output audio");
    systemAudioCheck_ = addControl(L"BUTTON", L"Capture output audio", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kSystemAudioCheckId);
    SendMessageW(systemAudioCheck_, BM_SETCHECK, settings_.captureSystemAudio ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(systemAudioCheck_, 0, 0, L"Records desktop/game audio from the selected output device.");
    y += kRowHeight;

    addRowLabel(L"Output device");
    outputDeviceCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kWideControlWidth, 140, kOutputDeviceComboId);
    addSettingHelp(outputDeviceCombo_, 0, 0, L"Output device used for system audio capture. Default follows the current Windows default output.");
    y += kRowHeight;

    addRowLabel(L"Output volume");
    outputVolumeEdit_ = addControl(L"EDIT", std::to_wstring(settings_.audioOutputVolumePercent).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kOutputVolumeEditId);
    addSettingHelp(outputVolumeEdit_, 0, 0, L"Volume applied to captured output audio before it is written to recordings and replay clips. 100 keeps the original level.");
    y += kRowHeight;

    addRowLabel(L"Microphone");
    microphoneCheck_ = addControl(L"BUTTON", L"Capture microphone", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kMicrophoneCheckId);
    SendMessageW(microphoneCheck_, BM_SETCHECK, settings_.captureMicrophone ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(microphoneCheck_, 0, 0, L"Records microphone audio from the selected input device.");
    y += kRowHeight;

    addRowLabel(L"Input device");
    inputDeviceCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kWideControlWidth, 140, kInputDeviceComboId);
    addSettingHelp(inputDeviceCombo_, 0, 0, L"Input device used for microphone capture. Default follows the current Windows default input.");
    y += kRowHeight;

    addRowLabel(L"Input volume");
    inputVolumeEdit_ = addControl(L"EDIT", std::to_wstring(settings_.audioInputVolumePercent).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kInputVolumeEditId);
    addSettingHelp(inputVolumeEdit_, 0, 0, L"Volume applied to captured microphone audio before it is written to recordings and replay clips. 100 keeps the original level.");
    loadAudioDeviceCombos();
}

void MainWindow::buildSettingsAdvancedPage() {
    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kControlWidth = 360;
    constexpr int kNumericWidth = 96;
    constexpr int kRowHeight = 38;
    constexpr int kSectionGap = 18;
    int y = 128;

    auto addRowLabel = [&](const wchar_t* text) {
        addControl(L"STATIC", text, 0, kX, y + 4, kLabelWidth, 22, -1);
    };
    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 36;
    };
    auto finishSection = [&]() {
        y += kSectionGap;
    };

    addSection(L"Encoder");
    addRowLabel(L"Profile");
    encoderProfileCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 100, kEncoderProfileComboId);
    SendMessageW(encoderProfileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Lowest GPU"));
    SendMessageW(encoderProfileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Balanced"));
    SendMessageW(encoderProfileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom"));
    SendMessageW(encoderProfileCombo_, CB_SETCURSEL, static_cast<WPARAM>(settings_.video.encoderProfile), 0);
    addSettingHelp(encoderProfileCombo_, 0, 0, L"Lowest GPU forces the lightest encoder settings. Balanced keeps a little more quality. Custom uses the controls below. GPU impact: profile-dependent.");
    y += kRowHeight;

    addRowLabel(L"Preset");
    encoderPresetCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 180, kEncoderPresetComboId);
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P1 fastest"));
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P2 faster"));
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P3 fast"));
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P4 medium"));
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P5 slow"));
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P6 slower"));
    SendMessageW(encoderPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P7 slowest"));
    SendMessageW(encoderPresetCombo_, CB_SETCURSEL, static_cast<WPARAM>(settings_.video.encoderPreset), 0);
    addSettingHelp(encoderPresetCombo_, 0, 0, L"Encoder preset speed/quality level. Higher numbers spend more encoder work for quality. On AMD (AMF) presets map to nearest Speed/Balanced/Quality. GPU impact: Low to High.");
    y += kRowHeight;

    addRowLabel(L"Tuning mode");
    encoderModeCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 160, kEncoderModeComboId);
    SendMessageW(encoderModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"High quality"));
    SendMessageW(encoderModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Low latency"));
    SendMessageW(encoderModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Ultra low latency"));
    SendMessageW(encoderModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Lossless"));
    SendMessageW(encoderModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Ultra high quality"));
    SendMessageW(encoderModeCombo_, CB_SETCURSEL, static_cast<WPARAM>(settings_.video.encoderMode), 0);
    addSettingHelp(encoderModeCombo_, 0, 0, L"Encoder tuning mode. Low-latency modes avoid extra buffering. Lossless and ultra-high-quality can be expensive. GPU impact: Low to High.");
    y += kRowHeight;

    addRowLabel(L"Multipass");
    encoderMultipassCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 110, kEncoderMultipassComboId);
    SendMessageW(encoderMultipassCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Disabled"));
    SendMessageW(encoderMultipassCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Quarter resolution"));
    SendMessageW(encoderMultipassCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Full resolution"));
    SendMessageW(encoderMultipassCombo_, CB_SETCURSEL, static_cast<WPARAM>(settings_.video.encoderMultipass), 0);
    addSettingHelp(encoderMultipassCombo_, 0, 0, L"Runs an extra analysis pass before encoding. Disable for lowest GPU use. GPU impact: Significant to High.");
    y += kRowHeight;

    addRowLabel(L"Lookahead");
    encoderLookaheadCheck_ = addControl(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, 120, 24, kEncoderLookaheadCheckId);
    SendMessageW(encoderLookaheadCheck_, BM_SETCHECK, settings_.video.encoderLookahead ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderLookaheadCheck_, 0, 0, L"Allows the encoder to inspect future frames for better rate control and B-frame choices. Disable for lowest latency and GPU use.");
    addControl(L"STATIC", L"Depth", 0, kControlX + 168, y + 4, 56, 22, -1);
    encoderLookaheadDepthEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.encoderLookaheadDepth).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX + 232, y, kNumericWidth, 24, kEncoderLookaheadDepthEditId);
    addSettingHelp(encoderLookaheadDepthEdit_, 0, 0, L"Number of future frames the encoder may analyze when lookahead is enabled. Higher depth can improve quality but increases latency and work.");
    y += kRowHeight;

    addRowLabel(L"Adaptive frames");
    encoderAdaptiveIFramesCheck_ = addControl(L"BUTTON", L"I-frames", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, 150, 24, kEncoderAdaptiveIFramesCheckId);
    SendMessageW(encoderAdaptiveIFramesCheck_, BM_SETCHECK, settings_.video.encoderAdaptiveIFrames ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderAdaptiveIFramesCheck_, 0, 0, L"Lets the encoder insert I-frames when scene changes need faster recovery. Can improve quality but may add encode spikes.");
    encoderAdaptiveBFramesCheck_ = addControl(L"BUTTON", L"B-frames", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX + 188, y, 150, 24, kEncoderAdaptiveBFramesCheckId);
    SendMessageW(encoderAdaptiveBFramesCheck_, BM_SETCHECK, settings_.video.encoderAdaptiveBFrames ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderAdaptiveBFramesCheck_, 0, 0, L"Lets the encoder vary B-frame placement based on content. Requires B-frames and can improve compression at extra encoder cost.");
    y += kRowHeight;

    addRowLabel(L"Spatial AQ");
    encoderSpatialAQCheck_ = addControl(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, 120, 24, kEncoderSpatialAQCheckId);
    SendMessageW(encoderSpatialAQCheck_, BM_SETCHECK, settings_.video.encoderSpatialAQ ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderSpatialAQCheck_, 0, 0, L"Redistributes bits within each frame to protect detailed areas. Improves perceived quality but adds encoder work.");
    addControl(L"STATIC", L"Strength", 0, kControlX + 168, y + 4, 70, 22, -1);
    encoderAQStrengthEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.encoderAQStrength).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX + 244, y, kNumericWidth, 24, kEncoderAQStrengthEditId);
    addSettingHelp(encoderAQStrengthEdit_, 0, 0, L"Spatial AQ strength from 1 to 15. Higher values favor detailed regions more aggressively and may cost more quality elsewhere.");
    y += kRowHeight;

    addRowLabel(L"Temporal AQ");
    encoderTemporalAQCheck_ = addControl(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kEncoderTemporalAQCheckId);
    SendMessageW(encoderTemporalAQCheck_, BM_SETCHECK, settings_.video.encoderTemporalAQ ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderTemporalAQCheck_, 0, 0, L"Adjusts quality across frames over time to preserve important motion. Can improve quality but increases encoder work.");
    y += kRowHeight;

    addRowLabel(L"B-frames");
    encoderBFramesCheck_ = addControl(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kEncoderBFramesCheckId);
    SendMessageW(encoderBFramesCheck_, BM_SETCHECK, settings_.video.encoderBFrames ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderBFramesCheck_, 0, 0, L"Enables bidirectionally predicted frames for better compression. Adds latency and encoder work, so keep off for lowest GPU use.");
    y += kRowHeight;

    addRowLabel(L"Keyframe sec");
    encoderGopSecondsEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.gopSeconds).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kEncoderGopSecondsEditId);
    addSettingHelp(encoderGopSecondsEdit_, 0, 0, L"Seconds between forced keyframes. Longer intervals reduce keyframe spikes but may delay replay trimming and scene recovery.");
    y += kRowHeight;

    addRowLabel(L"Reference frames");
    encoderReferenceFramesEdit_ = addControl(L"EDIT", std::to_wstring(settings_.video.encoderReferenceFrames).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kEncoderReferenceFramesEditId);
    addSettingHelp(encoderReferenceFramesEdit_, 0, 0, L"Number of reference frames used for prediction. One is lowest GPU and VRAM; higher values can improve compression.");
    y += kRowHeight;

    addRowLabel(L"Reorder delay");
    encoderZeroReorderDelayCheck_ = addControl(L"BUTTON", L"Zero reorder delay", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kEncoderZeroReorderDelayCheckId);
    SendMessageW(encoderZeroReorderDelayCheck_, BM_SETCHECK, settings_.video.encoderZeroReorderDelay ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(encoderZeroReorderDelayCheck_, 0, 0, L"Requests output packets without frame reordering delay. Useful for low latency and replay timing, but may limit B-frame behavior.");
    y += kRowHeight;
    finishSection();

    addSection(L"GPU");
    addRowLabel(L"Adaptive GPU");
    gpuAdaptiveCombo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kControlX, y, kControlWidth, 100, kGpuAdaptiveComboId);
    SendMessageW(gpuAdaptiveCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Disabled"));
    SendMessageW(gpuAdaptiveCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Conservative"));
    SendMessageW(gpuAdaptiveCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Aggressive"));
    SendMessageW(gpuAdaptiveCombo_, CB_SETCURSEL, static_cast<WPARAM>(settings_.gpu.adaptiveMode), 0);
    addSettingHelp(gpuAdaptiveCombo_, 0, 0, L"Drops capture work when encoder backlog appears to protect game frame time. Aggressive reacts earlier and may drop more frames.");
    y += kRowHeight;

    addRowLabel(L"Frame queue");
    gpuFrameQueueLimitEdit_ = addControl(L"EDIT", std::to_wstring(settings_.gpu.frameQueueLimit).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kGpuFrameQueueLimitEditId);
    addSettingHelp(gpuFrameQueueLimitEdit_, 0, 0, L"Maximum queued GPU frames before the encoder. Lower values reduce buffering and VRAM but can drop frames if encoding falls behind.");
    y += kRowHeight;

    addRowLabel(L"Idle frames");
    idleFrameCoalescingCheck_ = addControl(L"BUTTON", L"Lossless idle coalescing", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kIdleFrameCoalescingCheckId);
    SendMessageW(idleFrameCoalescingCheck_, BM_SETCHECK, settings_.gpu.allowIdleFrameSkipping ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(idleFrameCoalescingCheck_, 0, 0, L"Preserves the full 60 FPS timeline by extending unchanged samples instead of re-encoding the same captured texture. Periodic IDR heartbeats keep recordings and replays seekable.");
    y += kRowHeight;

    addRowLabel(L"WGC capture");
    wgcZeroCopyCheck_ = addControl(L"BUTTON", L"Zero-copy textures", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kWgcZeroCopyCheckId);
    SendMessageW(wgcZeroCopyCheck_, BM_SETCHECK, settings_.gpu.wgcZeroCopy ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(wgcZeroCopyCheck_, 0, 0, L"Uses the Windows Graphics Capture frame-pool texture directly when its native BGRA format can be submitted without conversion. Scaling or experimental format conversion may use a separate pooled output texture.");
    y += kRowHeight;

    addRowLabel(L"Monitor switches");
    stableMultimonitorFramesCheck_ = addControl(L"BUTTON", L"Stable multimonitor frames", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kStableMultimonitorFramesCheckId);
    SendMessageW(stableMultimonitorFramesCheck_, BM_SETCHECK, settings_.gpu.stableMultimonitorFrames ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(stableMultimonitorFramesCheck_, 0, 0, L"Waits for stable frames during monitor switches so recordings avoid transient wrong-monitor frames.");
    y += kRowHeight;

}

void MainWindow::buildSettingsSoundSeparationPage() {
    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kComboWidth = 360;
    constexpr int kRowHeight = 42;
    int y = 128;

    auto addRowLabel = [&](const wchar_t* text) {
        addControl(L"STATIC", text, 0, kX, y + 4, kLabelWidth, 22, -1);
    };
    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 36;
    };

    addSection(L"Sound seperation");
    addRowLabel(L"Mode");
    soundSeparationEnabledCheck_ = addControl(
        L"BUTTON",
        L"Enable sound seperation",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        kControlX,
        y,
        kComboWidth,
        24,
        kSoundSeparationEnabledCheckId);
    SendMessageW(soundSeparationEnabledCheck_, BM_SETCHECK, settings_.soundSeparationEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(soundSeparationEnabledCheck_, 0, 0, L"Turns app audio exclusion on or off. When off, Backtrack records normal system audio.");
    y += kRowHeight;

    addRowLabel(L"Active app");
    soundSeparationAppCombo_ = addControl(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | WS_TABSTOP,
        kControlX,
        y,
        kComboWidth,
        180,
        kSoundSeparationAppComboId);
    addSettingHelp(soundSeparationAppCombo_, 0, 0, L"Choose a running app to add it to sound seperation. Apps only affect clips while sound seperation is enabled and the app row is muted.");
    soundSeparationRefreshButton_ = addControl(
        L"BUTTON",
        L"Refresh",
        BS_PUSHBUTTON | WS_TABSTOP,
        kControlX + kComboWidth + 12,
        y - 2,
        88,
        30,
        kSoundSeparationRefreshButtonId);
    addSettingHelp(soundSeparationRefreshButton_, 0, 0, L"Scans audio sessions and visible running apps, then updates the app list.");
    soundSeparationManualButton_ = addControl(
        L"BUTTON",
        L"Manual",
        BS_PUSHBUTTON | WS_TABSTOP,
        kControlX + kComboWidth + 108,
        y - 2,
        88,
        30,
        kSoundSeparationManualButtonId);
    addSettingHelp(soundSeparationManualButton_, 0, 0, L"Select an executable manually when the app is not currently producing audio.");
    y += kRowHeight;

    addSection(L"Selected apps");
    soundSeparationRowsY_ = y;
    refreshSoundSeparationApps();
    rebuildSoundSeparationRows();
}

void MainWindow::buildSettingsGameIntegrationsPage() {
    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kControlWidth = 430;
    constexpr int kRowHeight = 42;
    int y = 128;

    auto addRowLabel = [&](const wchar_t* text) {
        addControl(L"STATIC", text, 0, kX, y + 4, kLabelWidth, 22, -1);
    };
    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 36;
    };

    addSection(L"Game integrations");
    addRowLabel(L"League of Legends");
    leagueKillReminderCheck_ = addControl(
        L"BUTTON",
        L"Beep on champion kills",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        kControlX,
        y,
        kControlWidth,
        24,
        kLeagueKillReminderCheckId);
    SendMessageW(leagueKillReminderCheck_, BM_SETCHECK, settings_.gameIntegrations.leagueOfLegendsKillReminder ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(leagueKillReminderCheck_, 0, 0, L"Polls League's local live-client API during active games. When your champion scores a kill, Backtrack beeps; if you save replay within 6 seconds, the clip is tagged as Kill clip.");
    y += kRowHeight;
}

void MainWindow::buildCapturePage() {
    settings_ = controller_.settings();

    constexpr int kX = 44;
    constexpr int kLabelWidth = 160;
    constexpr int kControlX = 224;
    constexpr int kControlWidth = 430;
    constexpr int kNumericWidth = 120;
    constexpr int kRowHeight = 40;
    constexpr int kSectionGap = 22;
    int y = 78;

    auto addRowLabel = [&](const wchar_t* text) {
        addControl(L"STATIC", text, 0, kX, y + 4, kLabelWidth, 22, -1);
    };
    auto addSection = [&](const wchar_t* text) {
        addSectionLabel(text, kX, y, 260);
        y += 38;
    };
    auto finishSection = [&]() {
        y += kSectionGap;
    };

    addSection(L"Recording");
    addRowLabel(L"Hotkey");
    recordHotkey_ = addControl(L"EDIT", hotkeyDisplay(recordHotkeyModifiers_, recordHotkeyVirtualKey_).c_str(), WS_BORDER | ES_READONLY | ES_CENTER | WS_TABSTOP, kControlX, y, kControlWidth, 24, kRecordHotkeyId);
    addSettingHelp(recordHotkey_, 0, 0, L"Keyboard shortcut for starting or stopping a normal recording. Focus this field and press the desired key combination.");
    y += kRowHeight;

    addRowLabel(L"Action");
    startStopButton_ = addControl(L"BUTTON", controller_.stats().recording ? L"Stop Recording" : L"Start Recording", BS_PUSHBUTTON | WS_TABSTOP, kControlX, y - 4, 170, 34, kStartStopButtonId);
    addControl(L"BUTTON", L"Recover Failed", BS_PUSHBUTTON | WS_TABSTOP, kControlX + 188, y - 4, 170, 34, kRecoverFailedRecordingButtonId);
    y += kRowHeight;
    finishSection();

    addSection(L"Replay");
    addRowLabel(L"Buffer");
    replayEnabledCheck_ = addControl(L"BUTTON", L"Enable instant replay buffer", BS_AUTOCHECKBOX | WS_TABSTOP, kControlX, y, kControlWidth, 24, kReplayEnabledId);
    SendMessageW(replayEnabledCheck_, BM_SETCHECK, settings_.replay.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    addSettingHelp(replayEnabledCheck_, 0, 0, L"Keeps a rolling encoded video/audio buffer so a recent clip can be saved without re-encoding.");
    y += kRowHeight;

    addRowLabel(L"Length seconds");
    replaySecondsEdit_ = addControl(L"EDIT", std::to_wstring(settings_.replay.seconds).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, kControlX, y, kNumericWidth, 24, kReplaySecondsEditId);
    addSettingHelp(replaySecondsEdit_, 0, 0, L"How many seconds of encoded packets are retained for instant replay. Longer buffers use more system memory and disk work on save, not more VRAM.");
    y += kRowHeight;

    addRowLabel(L"Save hotkey");
    replayHotkey_ = addControl(L"EDIT", hotkeyDisplay(replayHotkeyModifiers_, replayHotkeyVirtualKey_).c_str(), WS_BORDER | ES_READONLY | ES_CENTER | WS_TABSTOP, kControlX, y, kControlWidth, 24, kReplayHotkeyId);
    addSettingHelp(replayHotkey_, 0, 0, L"Keyboard shortcut for saving the current replay buffer. Focus this field and press the desired key combination.");
    y += kRowHeight;

    addRowLabel(L"Action");
    addControl(L"BUTTON", L"Save Replay", BS_PUSHBUTTON | WS_TABSTOP, kControlX, y - 4, 150, 34, kSaveReplayButtonId);
    addDirtySaveButton();
}

void MainWindow::buildStatsPage() {
    addSectionLabel(L"Runtime", 44, 78, 220);
    statsLabel_ = addControl(L"STATIC", L"", SS_LEFT, 44, 112, 350, 330, kStatsLabelId);
    addSectionLabel(L"Encoder", 464, 78, 220);
    capsLabel_ = addControl(L"STATIC", L"", SS_LEFT, 464, 112, 350, 330, kCapsLabelId);
    addSectionLabel(L"Recent Log", 44, 484, 220);
    addControl(L"BUTTON", L"Save Log", BS_PUSHBUTTON | WS_TABSTOP, 712, 480, 112, 30, kSaveLogButtonId);
    diagnosticLogLabel_ = addControl(L"STATIC", L"", SS_LEFT, 44, 518, 780, 150, -1);
    updateStats();
}

void MainWindow::buildClipsPage() {
    addSectionLabel(L"Library", 44, 78, 220);
    addControl(L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, 274, 76, 78, 28, kDeleteClipButtonId);
    addControl(L"BUTTON", L"Favorite", BS_PUSHBUTTON | WS_TABSTOP, 360, 76, 90, 28, kFavoriteClipButtonId);
    addControl(L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 458, 76, 86, 28, kRefreshClipsButtonId);
    listViewButton_ = addControl(L"BUTTON", L"List", BS_PUSHBUTTON | WS_TABSTOP, 628, 76, 76, 28, kClipListViewButtonId);
    galleryViewButton_ = addControl(L"BUTTON", L"Gallery", BS_PUSHBUTTON | WS_TABSTOP, 712, 76, 76, 28, kClipGalleryViewButtonId);
    clipList_ = addControl(L"LISTBOX", L"", WS_BORDER | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, 44, 112, 780, 370, kClipListId);
    setListItemHeightIfChanged(clipList_, clipListItemHeight_, libraryViewMode_ == LibraryViewMode::Gallery ? kGalleryClipMinimumItemHeight : kListClipItemHeight);
    if (clipList_) {
        SetWindowSubclass(clipList_, clipListSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    refreshClips();
}

void MainWindow::applyVisibleSettings() {
    const bool soundSeparationVisible = settingsCategory_ == SettingsCategory::SoundSeparation && soundSeparationAppCombo_;
    auto visibleSoundSeparationApps = settings_.soundSeparationApps;
    settings_ = controller_.settings();
    if (soundSeparationVisible) {
        settings_.soundSeparationApps = std::move(visibleSoundSeparationApps);
    }

    if (bitrateEdit_) {
        settings_.video.bitrateKbps = readUIntControl(bitrateEdit_, settings_.video.bitrateKbps);
    }
    if (fpsEdit_) {
        settings_.video.fps = std::max<uint32_t>(1, readUIntControl(fpsEdit_, settings_.video.fps));
    }
    if (resolutionModeCombo_) {
        const auto selected = SendMessageW(resolutionModeCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= static_cast<LRESULT>(ResolutionMode::Custom)) {
            settings_.video.resolutionMode = static_cast<ResolutionMode>(selected);
        }
    }
    if (widthEdit_) {
        settings_.video.width = std::max<uint32_t>(16, readUIntControl(widthEdit_, settings_.video.width));
    }
    if (heightEdit_) {
        settings_.video.height = std::max<uint32_t>(16, readUIntControl(heightEdit_, settings_.video.height));
    }
    if (followFocusedMonitorCheck_) {
        settings_.followFocusedMonitor = SendMessageW(followFocusedMonitorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (followMouseMonitorCheck_) {
        settings_.followMouseMonitor = settings_.followFocusedMonitor &&
            SendMessageW(followMouseMonitorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (codecCombo_) {
        const auto selected = SendMessageW(codecCombo_, CB_GETCURSEL, 0, 0);
        settings_.video.codec = selected == 1 ? VideoCodec::Hevc : VideoCodec::H264;
    }
    if (systemAudioCheck_) {
        settings_.captureSystemAudio = SendMessageW(systemAudioCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (microphoneCheck_) {
        settings_.captureMicrophone = SendMessageW(microphoneCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (outputDeviceCombo_) {
        settings_.audioOutputDeviceId = selectedDeviceId(outputDeviceCombo_, outputDevices_);
    }
    if (inputDeviceCombo_) {
        settings_.audioInputDeviceId = selectedDeviceId(inputDeviceCombo_, inputDevices_);
    }
    if (outputVolumeEdit_) {
        settings_.audioOutputVolumePercent = readUIntControl(outputVolumeEdit_, settings_.audioOutputVolumePercent);
    }
    if (inputVolumeEdit_) {
        settings_.audioInputVolumePercent = readUIntControl(inputVolumeEdit_, settings_.audioInputVolumePercent);
    }
    if (startWithWindowsCheck_) {
        settings_.startWithWindowsMinimized = SendMessageW(startWithWindowsCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (exitToTrayCheck_) {
        settings_.exitToTray = SendMessageW(exitToTrayCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderPresetCombo_) {
        const auto selected = SendMessageW(encoderPresetCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= 6) {
            settings_.video.encoderPreset = static_cast<EncoderPreset>(selected);
        }
    }
    if (encoderModeCombo_) {
        const auto selected = SendMessageW(encoderModeCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= 4) {
            settings_.video.encoderMode = static_cast<EncoderMode>(selected);
        }
    }
    if (encoderProfileCombo_) {
        const auto selected = SendMessageW(encoderProfileCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= 2) {
            settings_.video.encoderProfile = static_cast<EncoderProfile>(selected);
        }
    }
    if (encoderLookaheadCheck_) {
        settings_.video.encoderLookahead = SendMessageW(encoderLookaheadCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderLookaheadDepthEdit_) {
        settings_.video.encoderLookaheadDepth = std::min<uint32_t>(31, readUIntControl(encoderLookaheadDepthEdit_, settings_.video.encoderLookaheadDepth));
    }
    if (encoderSpatialAQCheck_) {
        settings_.video.encoderSpatialAQ = SendMessageW(encoderSpatialAQCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderAQStrengthEdit_) {
        settings_.video.encoderAQStrength = std::clamp<uint32_t>(readUIntControl(encoderAQStrengthEdit_, settings_.video.encoderAQStrength), 1, 15);
    }
    if (encoderTemporalAQCheck_) {
        settings_.video.encoderTemporalAQ = SendMessageW(encoderTemporalAQCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderMultipassCombo_) {
        const auto selected = SendMessageW(encoderMultipassCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= 2) {
            settings_.video.encoderMultipass = static_cast<EncoderMultipass>(selected);
        }
    }
    if (encoderBFramesCheck_) {
        settings_.video.encoderBFrames = SendMessageW(encoderBFramesCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderAdaptiveBFramesCheck_) {
        settings_.video.encoderAdaptiveBFrames = SendMessageW(encoderAdaptiveBFramesCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderAdaptiveIFramesCheck_) {
        settings_.video.encoderAdaptiveIFrames = SendMessageW(encoderAdaptiveIFramesCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderZeroReorderDelayCheck_) {
        settings_.video.encoderZeroReorderDelay = SendMessageW(encoderZeroReorderDelayCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (encoderGopSecondsEdit_) {
        settings_.video.gopSeconds = readUIntControl(encoderGopSecondsEdit_, settings_.video.gopSeconds);
    }
    if (encoderReferenceFramesEdit_) {
        settings_.video.encoderReferenceFrames = readUIntControl(encoderReferenceFramesEdit_, settings_.video.encoderReferenceFrames);
    }
    if (gpuAdaptiveCombo_) {
        const auto selected = SendMessageW(gpuAdaptiveCombo_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= 2) {
            settings_.gpu.adaptiveMode = static_cast<GpuAdaptiveMode>(selected);
        }
    }
    if (gpuFrameQueueLimitEdit_) {
        settings_.gpu.frameQueueLimit = readUIntControl(gpuFrameQueueLimitEdit_, settings_.gpu.frameQueueLimit);
    }
    if (idleFrameCoalescingCheck_) {
        settings_.gpu.allowIdleFrameSkipping =
            SendMessageW(idleFrameCoalescingCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (wgcZeroCopyCheck_) {
        settings_.gpu.wgcZeroCopy = SendMessageW(wgcZeroCopyCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (stableMultimonitorFramesCheck_) {
        settings_.gpu.stableMultimonitorFrames =
            SendMessageW(stableMultimonitorFramesCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (replayEnabledCheck_) {
        settings_.replay.enabled = SendMessageW(replayEnabledCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (replaySecondsEdit_) {
        settings_.replay.seconds = readUIntControl(replaySecondsEdit_, settings_.replay.seconds);
    }
    if (leagueKillReminderCheck_) {
        settings_.gameIntegrations.leagueOfLegendsKillReminder =
            SendMessageW(leagueKillReminderCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (soundSeparationEnabledCheck_) {
        settings_.soundSeparationEnabled = SendMessageW(soundSeparationEnabledCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (recordHotkey_) {
        settings_.hotkeys.startStopModifiers = recordHotkeyModifiers_;
        settings_.hotkeys.startStopVirtualKey = recordHotkeyVirtualKey_;
    }
    if (replayHotkey_) {
        settings_.hotkeys.saveReplayModifiers = replayHotkeyModifiers_;
        settings_.hotkeys.saveReplayVirtualKey = replayHotkeyVirtualKey_;
    }
    if (clipFolderEdit_) {
        const auto folder = trimText(readText(clipFolderEdit_));
        if (!folder.empty()) {
            std::error_code folderError;
            const std::filesystem::path folderPath(folder);
            if (std::filesystem::exists(folderPath, folderError) && !std::filesystem::is_directory(folderPath, folderError)) {
                setStatus(L"Clip folder must be a folder, not a file");
                return;
            }
            if (folderError) {
                setStatus(L"Clip folder could not be checked");
                return;
            }
            std::filesystem::create_directories(folderPath, folderError);
            if (folderError) {
                setStatus(L"Clip folder could not be created");
                return;
            }
            settings_.clipDirectory = folder;
        }
    }
    settings_.libraryGalleryView = libraryViewMode_ == LibraryViewMode::Gallery;

    settings_ = sanitizeSettings(settings_);
    if (controllerActionPending_.load()) {
        setStatus(L"Recorder is busy; try saving settings again after the current action finishes");
        playActionIndicator(MB_ICONHAND);
        return;
    }
    updateStartupRegistration();
    settingsStore_.save(settings_);
    clipManager_.setDirectory(settings_.clipDirectory);
    const bool hotkeysOk = hotkeys_.registerHotkeys(window_, settings_.hotkeys);
    const std::wstring hotkeyError = hotkeys_.lastErrorMessage();
    ControllerAction action;
    action.kind = ControllerActionKind::ApplySettings;
    action.settings = settings_;
    action.hotkeysOk = hotkeysOk;
    action.hotkeyError = hotkeyError;
    if (!queueControllerAction(std::move(action), L"Recorder is busy; capture settings were not applied")) {
        return;
    }
    setStatus(L"Settings saved; applying capture changes...");
    clearSettingsDirty();
}

void MainWindow::addDirtySaveButton() {
    saveSettingsButton_ = addControl(L"BUTTON", L"Save Settings", BS_PUSHBUTTON | WS_TABSTOP, 44, 654, 150, 32, kSaveSettingsButtonId);
    if (!layoutItems_.empty()) {
        layoutItems_.back().kind = LayoutItem::Kind::Footer;
    }
    clearSettingsDirty();
}

void MainWindow::markSettingsDirty() {
    if (buildingPage_) {
        return;
    }
    if (settingsDirty_) {
        return;
    }
    settingsDirty_ = true;
    if (saveSettingsButton_) {
        if (!IsWindowEnabled(saveSettingsButton_)) {
            EnableWindow(saveSettingsButton_, TRUE);
        }
        showWindowIfHidden(saveSettingsButton_);
    }
}

void MainWindow::clearSettingsDirty() {
    if (!settingsDirty_ && saveSettingsButton_ && !IsWindowVisible(saveSettingsButton_)) {
        return;
    }
    settingsDirty_ = false;
    if (saveSettingsButton_) {
        if (IsWindowEnabled(saveSettingsButton_)) {
            EnableWindow(saveSettingsButton_, FALSE);
        }
        if (IsWindowVisible(saveSettingsButton_)) {
            ShowWindow(saveSettingsButton_, SW_HIDE);
        }
    }
}

void MainWindow::addSettingHelp(HWND control, int x, int y, const std::wstring& text) {
    (void)x;
    (void)y;
    addStatusHelp(control, text);
}

void MainWindow::addStatusHelp(HWND control, const std::wstring& text) {
    if (control && !text.empty()) {
        statusHelpTexts_[control] = text;
    }
}

void MainWindow::updateStatusHelp(HWND hoveredControl) {
    HWND control = hoveredControl;
    while (control && control != window_ && control != pageHost_) {
        const auto help = statusHelpTexts_.find(control);
        if (help != statusHelpTexts_.end()) {
            if (hoveredHelpControl_ != control) {
                hoveredHelpControl_ = control;
                applyStatusText(help->second);
            }
            return;
        }
        control = GetParent(control);
    }

    if (hoveredHelpControl_) {
        hoveredHelpControl_ = nullptr;
        applyStatusText(statusText_);
    }
}

void MainWindow::applyStatusText(const std::wstring& status) {
    if (!status_) {
        return;
    }
    if (displayedStatusText_ == status) {
        return;
    }

    RECT client{};
    GetClientRect(window_, &client);
    const int contentWidth = std::max(1, static_cast<int>(client.right - client.left) - kOuterMargin * 2);
    const int previousHeight = statusHeightForText(contentWidth, displayedStatusText_);
    const int nextHeight = statusHeightForText(contentWidth, status);

    SetWindowTextW(status_, status.c_str());
    displayedStatusText_ = status;
    if (previousHeight != nextHeight) {
        layoutWindow();
    } else {
        InvalidateRect(status_, nullptr, FALSE);
    }
}

int MainWindow::statusHeightForWidth(int width) const {
    return statusHeightForText(width, currentStatusDisplayText());
}

std::wstring MainWindow::currentStatusDisplayText() const {
    if (hoveredHelpControl_) {
        const auto help = statusHelpTexts_.find(hoveredHelpControl_);
        if (help != statusHelpTexts_.end()) {
            return help->second;
        }
    }
    return statusText_;
}

int MainWindow::statusHeightForText(int width, const std::wstring& text) const {
    if (width <= 0) {
        return kStatusMinHeight;
    }

    HDC dc = GetDC(window_);
    if (!dc) {
        return kStatusMinHeight;
    }
    HGDIOBJ oldFont = nullptr;
    if (font_) {
        oldFont = SelectObject(dc, font_);
    }

    RECT rect{0, 0, std::max(24, width - kStatusPadding * 2), 0};
    DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);

    if (oldFont) {
        SelectObject(dc, oldFont);
    }
    ReleaseDC(window_, dc);
    return std::max(kStatusMinHeight, static_cast<int>(rect.bottom - rect.top) + 8);
}

void MainWindow::layoutWindow() {
    if (!window_) {
        return;
    }

    RECT client{};
    GetClientRect(window_, &client);
    const int clientWidth = std::max(1, static_cast<int>(client.right - client.left));
    const int clientHeight = std::max(1, static_cast<int>(client.bottom - client.top));
    const int contentWidth = std::max(1, clientWidth - kOuterMargin * 2);
    bool changed = false;

    if (!tabButtons_.empty()) {
        const int totalGap = kTabGap * (static_cast<int>(tabButtons_.size()) - 1);
        const int tabWidth = std::max(1, (contentWidth - totalGap) / static_cast<int>(tabButtons_.size()));
        int x = kOuterMargin;
        for (HWND button : tabButtons_) {
            changed = moveWindowIfChanged(button, x, kTabTop, tabWidth, kTabHeight) || changed;
            x += tabWidth + kTabGap;
        }
    }

    const int statusHeight = statusHeightForWidth(contentWidth);
    const int statusTop = std::max(
        kTabTop + kTabHeight + kPageGap + 40,
        clientHeight - kStatusBottomMargin - statusHeight);
    if (status_) {
        changed = moveWindowIfChanged(status_, kOuterMargin, statusTop, contentWidth, statusHeight) || changed;
    }

    const int pageTop = kTabTop + kTabHeight + kPageGap;
    const int pageBottom = std::max(pageTop + 40, statusTop - kPageGap);
    if (pageHost_) {
        changed = moveWindowIfChanged(pageHost_, kOuterMargin, pageTop, contentWidth, pageBottom - pageTop) || changed;
    }

    changed = layoutCurrentPage() || changed;
    if (changed) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

bool MainWindow::layoutCurrentPage() {
    if (!pageHost_) {
        return false;
    }

    RECT client{};
    GetClientRect(pageHost_, &client);
    const int viewportWidth = std::max(1, static_cast<int>(client.right - client.left));
    const int viewportHeight = std::max(1, static_cast<int>(client.bottom - client.top));
    const int designPageWidth = static_cast<int>(kPageRect.right - kPageRect.left);

    if (page_ == Page::Library && libraryViewMode_ == LibraryViewMode::Gallery && clipList_) {
        const int contentWidth = std::max(120, viewportWidth - kLayoutPadding * 2);
        const int yTitle = kLayoutPadding;
        const int yToolbar = yTitle + 36;
        const int controlHeight = 28;
        const int buttonWidth = 86;
        const int actionGap = 8;
        const int actionGroupGap = 36;
        const int viewButtonsLeft = kLayoutPadding + contentWidth - buttonWidth * 2 - actionGap;
        const int actionButtonWidth = std::clamp(
            (viewButtonsLeft - actionGroupGap - kLayoutPadding - actionGap * 2) / 3,
            56,
            92);
        const int actionsWidth = actionButtonWidth * 3 + actionGap * 2;
        const int actionsLeft = std::max(kLayoutPadding, viewButtonsLeft - actionGroupGap - actionsWidth);
        const int listTop = yToolbar + 34;
        const int listHeight = std::max(180, viewportHeight - listTop - kLayoutPadding);
        const int listWidth = contentWidth;
        const int galleryItemHeight = galleryItemHeightForWidth(listWidth);
        pageContentHeight_ = listTop + listHeight + kLayoutPadding;
        const int maxScroll = std::max(0, pageContentHeight_ - viewportHeight);
        pageScrollY_ = std::clamp(pageScrollY_, 0, maxScroll);

        bool changed = false;
        bool listChanged = false;
        auto move = [&](HWND control, int x, int y, int width, int height) {
            if (control) {
                changed = moveWindowIfChanged(control, x, y - pageScrollY_, width, height) || changed;
                changed = showWindowIfHidden(control) || changed;
            }
        };
        auto movePageControl = [&](size_t index, int x, int y, int width, int height) {
            if (index < pageControls_.size()) {
                move(pageControls_[index], x, y, width, height);
            }
        };

        movePageControl(0, kLayoutPadding, yTitle, 220, 24);
        move(GetDlgItem(pageHost_, kDeleteClipButtonId), actionsLeft, yTitle - 2, actionButtonWidth, controlHeight);
        move(GetDlgItem(pageHost_, kFavoriteClipButtonId), actionsLeft + (actionButtonWidth + actionGap), yTitle - 2, actionButtonWidth, controlHeight);
        move(GetDlgItem(pageHost_, kRefreshClipsButtonId), actionsLeft + (actionButtonWidth + actionGap) * 2, yTitle - 2, actionButtonWidth, controlHeight);
        move(listViewButton_, viewButtonsLeft, yTitle - 2, buttonWidth, controlHeight);
        move(galleryViewButton_, kLayoutPadding + contentWidth - buttonWidth, yTitle - 2, buttonWidth, controlHeight);

        listChanged = setListItemHeightIfChanged(clipList_, clipListItemHeight_, galleryItemHeight);
        move(clipList_, kLayoutPadding, listTop, listWidth, listHeight);

        SCROLLINFO scroll{};
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        scroll.nMin = 0;
        scroll.nMax = std::max(0, pageContentHeight_ - 1);
        scroll.nPage = static_cast<UINT>(viewportHeight);
        scroll.nPos = pageScrollY_;
        SetScrollInfo(pageHost_, SB_VERT, &scroll, TRUE);
        ShowScrollBar(pageHost_, SB_VERT, pageContentHeight_ > viewportHeight);
        if (listChanged) {
            InvalidateRect(clipList_, nullptr, FALSE);
        }
        if (changed || listChanged) {
            InvalidateRect(pageHost_, nullptr, FALSE);
        }
        return changed || listChanged;
    }

    struct Bounds {
        bool present = false;
        int left = std::numeric_limits<int>::max();
        int top = std::numeric_limits<int>::max();
        int right = std::numeric_limits<int>::min();
        int bottom = std::numeric_limits<int>::min();
    };

    Bounds groups[2];
    auto groupFor = [this, designPageWidth](const RECT& rect) {
        if (page_ == Page::Capture || page_ == Page::Settings) {
            return 0;
        }
        return static_cast<int>(rect.left) >= designPageWidth / 2 ? 1 : 0;
    };
    auto include = [](Bounds& bounds, const RECT& rect) {
        const int left = static_cast<int>(rect.left);
        const int top = static_cast<int>(rect.top);
        const int right = static_cast<int>(rect.right);
        const int bottom = static_cast<int>(rect.bottom);
        bounds.present = true;
        bounds.left = std::min(bounds.left, left);
        bounds.top = std::min(bounds.top, top);
        bounds.right = std::max(bounds.right, right);
        bounds.bottom = std::max(bounds.bottom, bottom);
    };

    for (const auto& item : layoutItems_) {
        if (!item.control || item.kind == LayoutItem::Kind::Footer) {
            continue;
        }
        include(groups[groupFor(item.design)], item.design);
    }

    const bool twoColumns =
        viewportWidth >= kTwoColumnMinimumWidth &&
        groups[0].present &&
        groups[1].present;

    struct Placement {
        HWND control = nullptr;
        RECT rect{};
        int windowHeight = 0;
    };
    std::vector<Placement> placements;
    int contentBottom = kLayoutPadding;

    auto scaleValue = [](int value, int target, int source) {
        return source > 0 ? MulDiv(value, target, source) : value;
    };
    auto addPlacement = [&](const LayoutItem& item, int x, int y, int width, int height) {
        RECT rect{x, y, x + width, y + height};
        const int windowHeight = item.windowHeight > 0 ? item.windowHeight : height;
        placements.push_back(Placement{item.control, rect, windowHeight});
        contentBottom = std::max(contentBottom, static_cast<int>(rect.bottom));
    };
    auto placeInColumn = [&](const LayoutItem& item, int group, int xOrigin, int columnWidth, int yOffset) {
        const Bounds& bounds = groups[group];
        const int itemLeft = static_cast<int>(item.design.left);
        const int itemTop = static_cast<int>(item.design.top);
        const int itemRight = static_cast<int>(item.design.right);
        const int itemBottom = static_cast<int>(item.design.bottom);
        const int sourceWidth = std::max(1, bounds.right - bounds.left);
        const int x = xOrigin + scaleValue(itemLeft - bounds.left, columnWidth, sourceWidth);
        const int width = std::max(24, scaleValue(itemRight - itemLeft, columnWidth, sourceWidth));
        const int y = itemTop + yOffset;
        const int height = std::max(1, itemBottom - itemTop);
        addPlacement(item, x, y, width, height);
    };

    if (twoColumns) {
        const int columnWidth = std::max(80, (viewportWidth - kLayoutPadding * 2 - kLayoutColumnGap) / 2);
        for (const auto& item : layoutItems_) {
            if (!item.control) {
                continue;
            }
            const int group = item.kind == LayoutItem::Kind::Footer ? 0 : groupFor(item.design);
            placeInColumn(
                item,
                group,
                kLayoutPadding + group * (columnWidth + kLayoutColumnGap),
                columnWidth,
                0);
        }
    } else {
        const int columnWidth = std::max(80, viewportWidth - kLayoutPadding * 2);
        int nextTop = kLayoutPadding;
        for (int group = 0; group < 2; ++group) {
            if (!groups[group].present) {
                continue;
            }
            const Bounds& bounds = groups[group];
            const int sourceWidth = std::max(1, bounds.right - bounds.left);
            const int sourceHeight = std::max(1, bounds.bottom - bounds.top);
            int groupBottom = nextTop;

            for (const auto& item : layoutItems_) {
                if (!item.control || item.kind == LayoutItem::Kind::Footer || groupFor(item.design) != group) {
                    continue;
                }
                const int itemLeft = static_cast<int>(item.design.left);
                const int itemTop = static_cast<int>(item.design.top);
                const int itemRight = static_cast<int>(item.design.right);
                const int itemBottom = static_cast<int>(item.design.bottom);
                const int x = kLayoutPadding + scaleValue(itemLeft - bounds.left, columnWidth, sourceWidth);
                const int width = std::max(24, scaleValue(itemRight - itemLeft, columnWidth, sourceWidth));
                const int y = nextTop + itemTop - bounds.top;
                const int height = std::max(1, itemBottom - itemTop);
                addPlacement(item, x, y, width, height);
                groupBottom = std::max(groupBottom, y + height);
            }
            nextTop += sourceHeight + kLayoutStackGap;
            contentBottom = std::max(contentBottom, groupBottom);
        }

        const int footerTop = contentBottom + 16;
        for (const auto& item : layoutItems_) {
            if (!item.control || item.kind != LayoutItem::Kind::Footer) {
                continue;
            }
            const int itemWidth = static_cast<int>(item.design.right - item.design.left);
            const int itemHeight = static_cast<int>(item.design.bottom - item.design.top);
            const int width = std::min(std::max(120, itemWidth), columnWidth);
            const int height = std::max(1, itemHeight);
            addPlacement(item, kLayoutPadding, footerTop, width, height);
        }
    }

    pageContentHeight_ = contentBottom + kLayoutPadding;
    const int maxScroll = std::max(0, pageContentHeight_ - viewportHeight);
    pageScrollY_ = std::clamp(pageScrollY_, 0, maxScroll);

    SCROLLINFO scroll{};
    scroll.cbSize = sizeof(scroll);
    scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0;
    scroll.nMax = std::max(0, pageContentHeight_ - 1);
    scroll.nPage = static_cast<UINT>(viewportHeight);
    scroll.nPos = pageScrollY_;
    SetScrollInfo(pageHost_, SB_VERT, &scroll, TRUE);
    ShowScrollBar(pageHost_, SB_VERT, pageContentHeight_ > viewportHeight);

    bool changed = false;
    for (const auto& placement : placements) {
        changed = moveWindowIfChanged(
            placement.control,
            placement.rect.left,
            placement.rect.top - pageScrollY_,
            placement.rect.right - placement.rect.left,
            placement.windowHeight) || changed;
    }
    if (changed) {
        InvalidateRect(pageHost_, nullptr, FALSE);
    }
    return changed;
}

void MainWindow::scrollPageTo(int position) {
    if (!pageHost_) {
        return;
    }
    RECT client{};
    GetClientRect(pageHost_, &client);
    const int viewportHeight = std::max(1, static_cast<int>(client.bottom - client.top));
    const int maxScroll = std::max(0, pageContentHeight_ - viewportHeight);
    const int clamped = std::clamp(position, 0, maxScroll);
    if (clamped != pageScrollY_) {
        pageScrollY_ = clamped;
        if (layoutCurrentPage()) {
            redrawWindowAndChildren(pageHost_, true);
        }
    }
}

void MainWindow::scrollPageBy(int delta) {
    scrollPageTo(pageScrollY_ + delta);
}

void MainWindow::scrollPageWheel(WPARAM wParam) {
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == 0) {
        return;
    }

    const int delta = static_cast<int>(GET_WHEEL_DELTA_WPARAM(wParam));
    if (lines == WHEEL_PAGESCROLL) {
        pageWheelRemainder_ += delta;
        const int pages = pageWheelRemainder_ / WHEEL_DELTA;
        pageWheelRemainder_ %= WHEEL_DELTA;
        if (pages != 0) {
            RECT client{};
            GetClientRect(pageHost_, &client);
            const int viewportHeight = std::max(1, static_cast<int>(client.bottom - client.top));
            scrollPageBy(-pages * viewportHeight);
        }
        return;
    }

    constexpr int kWheelPixelsPerLine = 16;
    pageWheelRemainder_ += delta * static_cast<int>(lines) * kWheelPixelsPerLine;
    const int pixels = pageWheelRemainder_ / WHEEL_DELTA;
    pageWheelRemainder_ %= WHEEL_DELTA;
    if (pixels != 0) {
        scrollPageBy(-pixels);
    }
}

bool MainWindow::handleClipListMouseWheel(WPARAM wParam) {
    if (!clipList_ || libraryViewMode_ != LibraryViewMode::Gallery) {
        return false;
    }

    clipListWheelRemainder_ += static_cast<int>(GET_WHEEL_DELTA_WPARAM(wParam));
    const int rows = clipListWheelRemainder_ / WHEEL_DELTA;
    clipListWheelRemainder_ %= WHEEL_DELTA;
    if (rows == 0) {
        return true;
    }

    const LRESULT countResult = SendMessageW(clipList_, LB_GETCOUNT, 0, 0);
    const LRESULT topResult = SendMessageW(clipList_, LB_GETTOPINDEX, 0, 0);
    if (countResult == LB_ERR || topResult == LB_ERR || countResult <= 0) {
        return true;
    }

    RECT client{};
    GetClientRect(clipList_, &client);
    const int listHeight = std::max(1, static_cast<int>(client.bottom - client.top));
    const int itemHeight = std::max(1, clipListItemHeight_);
    const int visibleRows = std::max(1, (listHeight + itemHeight - 1) / itemHeight);
    const int maxTop = std::max(0, static_cast<int>(countResult) - visibleRows);
    const int top = static_cast<int>(topResult);
    const int targetTop = std::clamp(top - rows, 0, maxTop);
    if (targetTop != top) {
        SendMessageW(clipList_, LB_SETTOPINDEX, static_cast<WPARAM>(targetTop), 0);
        InvalidateRect(clipList_, nullptr, FALSE);
    }
    return true;
}

void MainWindow::updateResolutionControls() {
    if (!resolutionModeCombo_) {
        return;
    }

    const auto selected = SendMessageW(resolutionModeCombo_, CB_GETCURSEL, 0, 0);
    const ResolutionMode mode =
        selected >= 0 && selected <= static_cast<LRESULT>(ResolutionMode::Custom)
            ? static_cast<ResolutionMode>(selected)
            : ResolutionMode::Native;

    uint32_t presetWidth = 0;
    uint32_t presetHeight = 0;
    if (resolutionPresetSize(mode, presetWidth, presetHeight)) {
        setText(widthEdit_, std::to_wstring(presetWidth));
        setText(heightEdit_, std::to_wstring(presetHeight));
    }

    const bool followFocusedMonitor =
        followFocusedMonitorCheck_ && SendMessageW(followFocusedMonitorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (followMouseMonitorCheck_) {
        EnableWindow(followMouseMonitorCheck_, followFocusedMonitor ? TRUE : FALSE);
        if (!followFocusedMonitor) {
            SendMessageW(followMouseMonitorCheck_, BM_SETCHECK, BST_UNCHECKED, 0);
        }
    }
    const BOOL editableSize =
        (mode == ResolutionMode::Custom || (followFocusedMonitor && mode == ResolutionMode::Native)) ? TRUE : FALSE;
    EnableWindow(widthEdit_, editableSize);
    EnableWindow(heightEdit_, editableSize);
}

void MainWindow::loadAudioDeviceCombos() {
    outputDevices_ = WasapiCapture::enumerateDevices(AudioTrack::System);
    inputDevices_ = WasapiCapture::enumerateDevices(AudioTrack::Microphone);

    if (outputDeviceCombo_) {
        ScopedRedrawLock redraw(outputDeviceCombo_);
        SendMessageW(outputDeviceCombo_, CB_RESETCONTENT, 0, 0);
        SendMessageW(outputDeviceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Default output device"));
        for (const auto& device : outputDevices_) {
            SendMessageW(outputDeviceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.name.c_str()));
        }
        SendMessageW(outputDeviceCombo_, CB_SETCURSEL, comboIndexForDevice(outputDevices_, settings_.audioOutputDeviceId), 0);
    }

    if (inputDeviceCombo_) {
        ScopedRedrawLock redraw(inputDeviceCombo_);
        SendMessageW(inputDeviceCombo_, CB_RESETCONTENT, 0, 0);
        SendMessageW(inputDeviceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Default input device"));
        for (const auto& device : inputDevices_) {
            SendMessageW(inputDeviceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.name.c_str()));
        }
        SendMessageW(inputDeviceCombo_, CB_SETCURSEL, comboIndexForDevice(inputDevices_, settings_.audioInputDeviceId), 0);
    }
}

void MainWindow::refreshSoundSeparationApps() {
    std::map<std::wstring, AudioSessionAppInfo> appsByPath;
    auto addApps = [&](std::vector<AudioSessionAppInfo> apps) {
        for (auto& app : apps) {
            const std::wstring key = executableKey(app.executablePath);
            if (key.empty() || appsByPath.contains(key)) {
                continue;
            }
            appsByPath.emplace(key, std::move(app));
        }
    };
    addApps(WasapiCapture::enumerateAudioSessionApps());
    addApps(WasapiCapture::enumerateOpenApps());

    soundSeparationAvailableApps_.clear();
    soundSeparationAvailableApps_.reserve(appsByPath.size());
    for (auto& [_, app] : appsByPath) {
        soundSeparationAvailableApps_.push_back(std::move(app));
    }
    std::sort(soundSeparationAvailableApps_.begin(), soundSeparationAvailableApps_.end(), [](const AudioSessionAppInfo& left, const AudioSessionAppInfo& right) {
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    });

    if (soundSeparationAppCombo_) {
        ScopedRedrawLock redraw(soundSeparationAppCombo_);
        SendMessageW(soundSeparationAppCombo_, CB_RESETCONTENT, 0, 0);
        SendMessageW(soundSeparationAppCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Choose active app"));
        for (const auto& app : soundSeparationAvailableApps_) {
            std::wstring label = app.name.empty() ? appNameFromPath(app.executablePath) : app.name;
            const std::wstring fileName = app.executablePath.filename().wstring();
            if (!fileName.empty() && label.find(fileName) == std::wstring::npos) {
                label += L" (" + fileName + L")";
            }
            SendMessageW(soundSeparationAppCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(soundSeparationAppCombo_, CB_SETCURSEL, 0, 0);
    }

    if (!buildingPage_) {
        setStatus(L"Found " + std::to_wstring(soundSeparationAvailableApps_.size()) +
                  (soundSeparationAvailableApps_.size() == 1 ? L" selectable app" : L" selectable apps"));
    }
}

void MainWindow::addSelectedSoundSeparationApp() {
    if (!soundSeparationAppCombo_) {
        return;
    }
    const auto selected = SendMessageW(soundSeparationAppCombo_, CB_GETCURSEL, 0, 0);
    if (selected <= 0) {
        return;
    }
    const size_t index = static_cast<size_t>(selected - 1);
    if (index >= soundSeparationAvailableApps_.size()) {
        return;
    }
    const auto& app = soundSeparationAvailableApps_[index];
    addSoundSeparationApp(app.name, app.executablePath);
    SendMessageW(soundSeparationAppCombo_, CB_SETCURSEL, 0, 0);
}

void MainWindow::addManualSoundSeparationApp() {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        setStatus(L"Manual app picker could not open");
        return;
    }

    COMDLG_FILTERSPEC filters[] = {
        {L"Applications", L"*.exe"},
        {L"All files", L"*.*"},
    };
    dialog->SetTitle(L"Choose app executable");
    dialog->SetFileTypes(static_cast<UINT>(_countof(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"exe");

    hr = dialog->Show(window_);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return;
    }
    if (FAILED(hr)) {
        setStatus(L"Manual app picker failed");
        return;
    }

    Microsoft::WRL::ComPtr<IShellItem> item;
    hr = dialog->GetResult(&item);
    if (FAILED(hr) || !item) {
        setStatus(L"No executable selected");
        return;
    }

    PWSTR rawPath = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    if (FAILED(hr) || !rawPath) {
        CoTaskMemFree(rawPath);
        setStatus(L"Selected app path could not be read");
        return;
    }

    std::filesystem::path executablePath(rawPath);
    CoTaskMemFree(rawPath);
    addSoundSeparationApp(appNameFromPath(executablePath), executablePath);
}

void MainWindow::addSoundSeparationApp(std::wstring name, std::filesystem::path executablePath) {
    if (executablePath.empty()) {
        setStatus(L"Selected app has no executable path");
        return;
    }

    const std::wstring key = executableKey(executablePath);
    for (auto& app : settings_.soundSeparationApps) {
        if (executableKey(app.executablePath) == key) {
            app.muted = true;
            if (app.name.empty()) {
                app.name = name.empty() ? appNameFromPath(executablePath) : std::move(name);
            }
            rebuildSoundSeparationRows();
            markSettingsDirty();
            setStatus(L"App already listed; muted");
            return;
        }
    }

    AppSettings::SoundSeparationApp app;
    app.name = name.empty() ? appNameFromPath(executablePath) : std::move(name);
    app.executablePath = std::move(executablePath);
    app.muted = true;
    settings_.soundSeparationApps.push_back(std::move(app));
    rebuildSoundSeparationRows();
    markSettingsDirty();
    setStatus(L"App added to sound seperation");
}

void MainWindow::rebuildSoundSeparationRows() {
    destroySoundSeparationRows();
    if (!pageHost_ || soundSeparationRowsY_ <= 0) {
        return;
    }

    constexpr int kX = 224;
    constexpr int kMicSize = 30;
    constexpr int kIconSize = 24;
    constexpr int kNameWidth = 300;
    constexpr int kMutedWidth = 70;
    constexpr int kRemoveWidth = 88;
    constexpr int kRowHeight = 42;
    int y = soundSeparationRowsY_;

    if (settings_.soundSeparationApps.empty()) {
        SoundSeparationRowControls row;
        row.nameLabel = addControl(L"STATIC", L"No apps selected", SS_LEFT, kX, y + 4, 360, 22, -1);
        soundSeparationRows_.push_back(row);
        layoutCurrentPage();
        redrawWindowAndChildren(pageHost_);
        return;
    }

    for (size_t index = 0; index < settings_.soundSeparationApps.size(); ++index) {
        const auto& app = settings_.soundSeparationApps[index];
        SoundSeparationRowControls row;
        row.micButton = addControl(
            L"BUTTON",
            L"",
            BS_PUSHBUTTON | WS_TABSTOP,
            kX,
            y - 2,
            kMicSize,
            kMicSize,
            kSoundSeparationMuteButtonBaseId + static_cast<int>(index));
        addSettingHelp(row.micButton, 0, 0, app.muted ? L"Unmute this app in future clips." : L"Mute this app in future clips.");

        row.iconControl = addControl(
            L"STATIC",
            L"",
            SS_ICON,
            kX + 44,
            y,
            kIconSize,
            kIconSize,
            -1);
        row.icon = loadExecutableIcon(app.executablePath);
        if (row.icon) {
            SendMessageW(row.iconControl, STM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(row.icon));
        }

        const std::wstring name = app.name.empty() ? appNameFromPath(app.executablePath) : app.name;
        row.nameLabel = addControl(
            L"STATIC",
            name.c_str(),
            SS_LEFT | SS_NOPREFIX,
            kX + 80,
            y + 4,
            kNameWidth,
            22,
            -1);
        addSettingHelp(row.nameLabel, 0, 0, app.executablePath.wstring());

        row.mutedLabel = addControl(
            L"STATIC",
            app.muted ? L"MUTED" : L"",
            SS_LEFT,
            kX + 396,
            y + 4,
            kMutedWidth,
            22,
            -1);

        row.removeButton = addControl(
            L"BUTTON",
            L"Remove",
            BS_PUSHBUTTON | WS_TABSTOP,
            kX + 484,
            y - 2,
            kRemoveWidth,
            30,
            kSoundSeparationRemoveButtonBaseId + static_cast<int>(index));
        addSettingHelp(row.removeButton, 0, 0, L"Remove this app from sound seperation.");

        soundSeparationRows_.push_back(row);
        y += kRowHeight;
    }

    layoutCurrentPage();
    redrawWindowAndChildren(pageHost_);
}

void MainWindow::destroySoundSeparationRows() {
    auto removeControl = [&](HWND control) {
        if (!control) {
            return;
        }
        statusHelpTexts_.erase(control);
        pageControls_.erase(std::remove(pageControls_.begin(), pageControls_.end(), control), pageControls_.end());
        layoutItems_.erase(
            std::remove_if(
                layoutItems_.begin(),
                layoutItems_.end(),
                [control](const LayoutItem& item) {
                    return item.control == control;
                }),
            layoutItems_.end());
        DestroyWindow(control);
    };

    for (auto& row : soundSeparationRows_) {
        removeControl(row.micButton);
        removeControl(row.iconControl);
        removeControl(row.nameLabel);
        removeControl(row.mutedLabel);
        removeControl(row.removeButton);
        if (row.icon) {
            DestroyIcon(row.icon);
            row.icon = nullptr;
        }
    }
    soundSeparationRows_.clear();
}

void MainWindow::releaseSoundSeparationRowIcons() {
    for (auto& row : soundSeparationRows_) {
        if (row.icon) {
            DestroyIcon(row.icon);
            row.icon = nullptr;
        }
    }
    soundSeparationRows_.clear();
}

void MainWindow::toggleSoundSeparationApp(size_t index) {
    if (index >= settings_.soundSeparationApps.size()) {
        return;
    }
    settings_.soundSeparationApps[index].muted = !settings_.soundSeparationApps[index].muted;
    rebuildSoundSeparationRows();
    markSettingsDirty();
    setStatus(settings_.soundSeparationApps[index].muted ? L"App muted" : L"App unmuted");
}

void MainWindow::removeSoundSeparationApp(size_t index) {
    if (index >= settings_.soundSeparationApps.size()) {
        return;
    }
    settings_.soundSeparationApps.erase(settings_.soundSeparationApps.begin() + static_cast<std::ptrdiff_t>(index));
    rebuildSoundSeparationRows();
    markSettingsDirty();
    setStatus(L"App removed from sound seperation");
}

bool MainWindow::isSoundSeparationMutedLabel(HWND control) const {
    return std::any_of(soundSeparationRows_.begin(), soundSeparationRows_.end(), [control](const SoundSeparationRowControls& row) {
        return row.mutedLabel == control;
    });
}

void MainWindow::updateStartupRegistration() {
    updateWindowsStartupRegistration(settings_.startWithWindowsMinimized);
}

void MainWindow::browseClipFolder() {
    wchar_t displayName[MAX_PATH]{};
    BROWSEINFOW browse{};
    browse.hwndOwner = window_;
    browse.pszDisplayName = displayName;
    browse.lpszTitle = L"Choose where Backtrack saves clips";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item) {
        return;
    }

    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(item, path)) {
        setText(clipFolderEdit_, path);
        markSettingsDirty();
    }
    CoTaskMemFree(item);
}

void MainWindow::saveLog() {
    const auto source = Logger::instance().currentPath();
    if (source.empty()) {
        setStatus(L"No log file available to save");
        return;
    }

    std::error_code existsError;
    if (!std::filesystem::exists(source, existsError) || existsError) {
        setStatus(L"Log file not found");
        return;
    }

    wchar_t fileName[MAX_PATH]{};
    wcscpy_s(fileName, L"backtrack-log.txt");

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"Log files (*.txt;*.log)\0*.txt;*.log\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"Save log copy";
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&dialog)) {
        return;
    }

    std::error_code copyError;
    std::filesystem::copy_file(
        source,
        std::filesystem::path(fileName),
        std::filesystem::copy_options::overwrite_existing,
        copyError);
    if (copyError) {
        setStatus(L"Could not save log: " + utf8ToWide(copyError.message()));
        return;
    }
    setStatus(std::wstring(L"Log saved to ") + fileName);
}

void MainWindow::updateStats() {
    const auto stats = controller_.stats();
    if (statsLabel_) {
        std::wstringstream stream;
        stream << L"Recording: " << (stats.recording ? L"yes" : L"no") << L"\r\n";
        stream << L"Replay buffer: " << (stats.replayEnabled ? L"enabled" : L"disabled") << L"\r\n";
        stream << L"Selected backend: " << captureBackendDisplayName(stats.selectedCaptureBackend) << L"\r\n";
        stream << L"Active backend: ";
        if (stats.captureBackendActive) {
            stream << captureBackendDisplayName(stats.activeCaptureBackend);
        } else {
            stream << L"inactive";
        }
        if (stats.captureBackendFallbackUsed) {
            stream << L" (fallback)";
        }
        stream << L"\r\n";
        if (!stats.captureBackendStatus.empty()) {
            stream << L"Backend status: " << stats.captureBackendStatus << L"\r\n";
        }
        stream << L"Capture size: " << stats.captureWidth << L"x" << stats.captureHeight << L"\r\n";
        stream << L"Encode size: " << stats.encodeWidth << L"x" << stats.encodeHeight << L"\r\n";
        stream << L"Timeline intervals: " << stats.capturedFrames << L"\r\n";
        stream << L"New source frames: " << stats.sourceFrames << L"\r\n";
        stream << L"Cadence duplicates: " << stats.cadenceDuplicateFrames << L"\r\n";
        stream << L"Catch-up duplicates: " << stats.catchUpDuplicateFrames << L"\r\n";
        stream << L"Coalesced idle intervals: " << stats.coalescedIdleIntervals << L"\r\n";
        stream << L"Dropped frames: " << stats.droppedFrames + stats.encoder.droppedFrames << L"\r\n";
        stream << L"GPU protection drops: " << stats.gpuProtectionDrops << L"\r\n";
        stream << L"Frame queue depth: " << stats.encoder.queueDepth << L"\r\n";
        stream << L"Encoder submissions: " << stats.encoder.submittedFrames << L"\r\n";
        stream << L"Encoded frames: " << stats.encoder.encodedFrames << L"\r\n";
        stream << L"Encoded keyframes: " << stats.encoder.keyFrames << L"\r\n";
        stream << L"Replay packets: " << stats.replayVideoPackets << L"\r\n";
        stream << L"Replay keyframes: " << stats.replayKeyFrames << L"\r\n";
        stream << L"Average encode: " << stats.encoder.averageEncodeMs << L" ms";
        setText(statsLabel_, stream.str());
    }

    if (capsLabel_) {
        const auto caps = controller_.encoderCapabilities();
        std::wstringstream stream;
        stream << L"Adapter: " << caps.adapterName << L"\r\n";
        const std::wstring backendName = caps.backendName.empty() ? L"Hardware encoder" : caps.backendName;
        stream << backendName << L": " << (caps.available ? L"available" : L"unavailable") << L"\r\n";
        stream << L"H.264: " << (caps.h264 ? L"yes" : L"no") << L"\r\n";
        stream << L"HEVC: " << (caps.hevc ? L"yes" : L"no") << L"\r\n";
        stream << L"Max size: " << caps.maxWidth << L"x" << caps.maxHeight << L"\r\n";
        stream << L"Profile: " << encoderProfileName(caps.effective.profile) << L"\r\n";
        stream << L"Preset/mode: " << encoderPresetName(caps.effective.preset)
               << L" / " << encoderModeName(caps.effective.mode) << L"\r\n";
        stream << L"Lookahead: " << yesNo(caps.effective.lookahead)
               << L" depth " << caps.effective.lookaheadDepth << L"\r\n";
        stream << L"Spatial AQ: " << yesNo(caps.effective.spatialAQ)
               << L" strength " << caps.effective.aqStrength << L"\r\n";
        stream << L"Temporal AQ: " << yesNo(caps.effective.temporalAQ) << L"\r\n";
        stream << L"Multipass: " << encoderMultipassName(caps.effective.multipass) << L"\r\n";
        stream << L"B-frames: " << yesNo(caps.effective.bFrames)
               << L", adaptive B: " << yesNo(caps.effective.adaptiveBFrames)
               << L", adaptive I: " << yesNo(caps.effective.adaptiveIFrames) << L"\r\n";
        stream << L"Reference frames: " << caps.effective.referenceFrames
               << L" (multiple refs: " << yesNo(caps.multipleReferenceFrames) << L")\r\n";
        stream << L"Zero reorder delay: " << yesNo(caps.effective.zeroReorderDelay) << L"\r\n";
        stream << caps.detail;
        setText(capsLabel_, stream.str());
    }

    if (diagnosticLogLabel_) {
        const auto lines = Logger::instance().recentLines(7);
        std::wstringstream stream;
        if (lines.empty()) {
            stream << L"No log lines available";
        } else {
            // Collapse runs of identical consecutive lines into "line (Nx)".
            // Compare the message portion after the "[timestamp] [LEVEL] "
            // prefix so repeats logged at different times still collapse.
            auto messageKey = [](const std::wstring& line) -> std::wstring {
                size_t pos = 0;
                for (int bracket = 0; bracket < 2; ++bracket) {
                    const size_t close = line.find(L']', pos);
                    if (close == std::wstring::npos) {
                        return line;
                    }
                    pos = close + 1;
                    while (pos < line.size() && line[pos] == L' ') {
                        ++pos;
                    }
                }
                return line.substr(pos);
            };
            for (size_t i = 0; i < lines.size();) {
                const std::wstring key = messageKey(lines[i]);
                size_t count = 1;
                while (i + count < lines.size() && messageKey(lines[i + count]) == key) {
                    ++count;
                }
                stream << lines[i];
                if (count > 1) {
                    stream << L" (" << count << L"x)";
                }
                stream << L"\r\n";
                i += count;
            }
        }
        setText(diagnosticLogLabel_, stream.str());
    }
}

void MainWindow::setLibraryViewMode(LibraryViewMode mode) {
    if (libraryViewMode_ == mode && clipList_) {
        return;
    }
    libraryViewMode_ = mode;
    selectedClipIndex_ = static_cast<size_t>(-1);
    clipListWheelRemainder_ = 0;
    settings_ = controller_.settings();
    settings_.libraryGalleryView = mode == LibraryViewMode::Gallery;
    settingsStore_.save(settings_);
    Logger::instance().info(std::wstring(L"Library view changed to ") + (mode == LibraryViewMode::Gallery ? L"gallery" : L"list"));
    if (clipList_) {
        setListItemHeightIfChanged(clipList_, clipListItemHeight_, mode == LibraryViewMode::Gallery ? kGalleryClipMinimumItemHeight : kListClipItemHeight);
        refreshClips();
        layoutCurrentPage();
        InvalidateRect(clipList_, nullptr, FALSE);
    }
}

void MainWindow::clearClipThumbnails() {
    thumbnailGeneration_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::scoped_lock lock(thumbnailQueueMutex_);
        thumbnailQueue_.clear();
    }
    clipThumbnails_.clear();
}

void MainWindow::releaseClipThumbnailCache() {
    clearClipThumbnails();
    for (auto& [key, entry] : thumbnailCache_) {
        if (entry.bitmap) {
            DeleteObject(entry.bitmap);
        }
    }
    thumbnailCache_.clear();
}

void MainWindow::pruneClipThumbnailCache() {
    std::unordered_set<std::wstring> liveKeys;
    liveKeys.reserve(clips_.size());
    for (const auto& clip : clips_) {
        liveKeys.insert(thumbnailCacheKey(clip.path));
    }

    for (auto it = thumbnailCache_.begin(); it != thumbnailCache_.end();) {
        if (liveKeys.contains(it->first)) {
            ++it;
            continue;
        }
        if (it->second.bitmap) {
            DeleteObject(it->second.bitmap);
        }
        it = thumbnailCache_.erase(it);
    }
}

void MainWindow::loadClipThumbnails() {
    clearClipThumbnails();
    if (libraryViewMode_ != LibraryViewMode::Gallery) {
        return;
    }
    pruneClipThumbnailCache();
    clipThumbnails_.resize(clips_.size());

    std::deque<ThumbnailRequest> requests;
    const uint64_t generation = thumbnailGeneration_.load(std::memory_order_acquire);
    for (size_t index = 0; index < clips_.size(); ++index) {
        const auto& clip = clips_[index];
        const std::wstring key = thumbnailCacheKey(clip.path);
        auto cache = thumbnailCache_.find(key);
        if (cache != thumbnailCache_.end()) {
            auto& entry = cache->second;
            if (entry.bytes == clip.bytes && entry.modifiedTime == clip.modifiedTime) {
                clipThumbnails_[index].bitmap = entry.bitmap;
                clipThumbnails_[index].unavailable = entry.unavailable;
                continue;
            }
            if (entry.bitmap) {
                DeleteObject(entry.bitmap);
            }
            thumbnailCache_.erase(cache);
        }

        clipThumbnails_[index].loading = true;
        ThumbnailRequest request;
        request.generation = generation;
        request.clipIndex = index;
        request.path = clip.path;
        request.modifiedTime = clip.modifiedTime;
        request.bytes = clip.bytes;
        requests.push_back(std::move(request));
    }

    if (requests.empty()) {
        return;
    }

    startThumbnailWorker();
    {
        std::scoped_lock lock(thumbnailQueueMutex_);
        if (thumbnailWorkerStopping_) {
            return;
        }
        for (auto& request : requests) {
            thumbnailQueue_.push_back(std::move(request));
        }
    }
    thumbnailQueueCv_.notify_one();
}

void MainWindow::startThumbnailWorker() {
    if (thumbnailWorker_.joinable()) {
        return;
    }

    {
        std::scoped_lock lock(thumbnailQueueMutex_);
        thumbnailWorkerStopping_ = false;
        thumbnailQueue_.clear();
    }
    thumbnailWorker_ = std::thread(&MainWindow::thumbnailWorkerLoop, this);
}

void MainWindow::stopThumbnailWorker() {
    thumbnailGeneration_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::scoped_lock lock(thumbnailQueueMutex_);
        thumbnailWorkerStopping_ = true;
        thumbnailQueue_.clear();
    }
    thumbnailQueueCv_.notify_all();
    if (thumbnailWorker_.joinable()) {
        thumbnailWorker_.join();
    }

    MSG message{};
    while (window_ && PeekMessageW(&message, window_, kClipThumbnailReadyMessage, kClipThumbnailReadyMessage, PM_REMOVE)) {
        auto* result = reinterpret_cast<ThumbnailResult*>(message.lParam);
        if (result) {
            if (result->bitmap) {
                DeleteObject(result->bitmap);
            }
            delete result;
        }
    }
}

void MainWindow::thumbnailWorkerLoop() {
    setThreadDescriptionSafe(L"Backtrack thumbnails");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    for (;;) {
        ThumbnailRequest request;
        {
            std::unique_lock lock(thumbnailQueueMutex_);
            thumbnailQueueCv_.wait(lock, [&] {
                return thumbnailWorkerStopping_ || !thumbnailQueue_.empty();
            });
            if (thumbnailWorkerStopping_ && thumbnailQueue_.empty()) {
                break;
            }
            request = std::move(thumbnailQueue_.front());
            thumbnailQueue_.pop_front();
        }

        if (request.generation != thumbnailGeneration_.load(std::memory_order_acquire)) {
            continue;
        }

        HBITMAP bitmap = loadShellThumbnail(request.path, kGalleryThumbnailWidth, kGalleryThumbnailHeight);
        bool stopping = false;
        {
            std::scoped_lock lock(thumbnailQueueMutex_);
            stopping = thumbnailWorkerStopping_;
        }
        if (stopping || request.generation != thumbnailGeneration_.load(std::memory_order_acquire)) {
            if (bitmap) {
                DeleteObject(bitmap);
            }
            continue;
        }

        auto result = std::make_unique<ThumbnailResult>();
        result->generation = request.generation;
        result->clipIndex = request.clipIndex;
        result->path = std::move(request.path);
        result->modifiedTime = request.modifiedTime;
        result->bytes = request.bytes;
        result->bitmap = bitmap;

        auto* rawResult = result.release();
        if (!window_ ||
            !IsWindow(window_) ||
            !PostMessageW(window_, kClipThumbnailReadyMessage, 0, reinterpret_cast<LPARAM>(rawResult))) {
            if (rawResult->bitmap) {
                DeleteObject(rawResult->bitmap);
            }
            delete rawResult;
        }
    }

    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
}

void MainWindow::handleClipThumbnailReady(ThumbnailResult& result) {
    auto deleteResultBitmap = [&] {
        if (result.bitmap) {
            DeleteObject(result.bitmap);
            result.bitmap = nullptr;
        }
    };

    if (result.generation != thumbnailGeneration_.load(std::memory_order_acquire) ||
        libraryViewMode_ != LibraryViewMode::Gallery ||
        result.clipIndex >= clips_.size() ||
        result.clipIndex >= clipThumbnails_.size()) {
        deleteResultBitmap();
        return;
    }

    const auto& clip = clips_[result.clipIndex];
    const std::wstring key = thumbnailCacheKey(result.path);
    if (thumbnailCacheKey(clip.path) != key ||
        clip.bytes != result.bytes ||
        clip.modifiedTime != result.modifiedTime) {
        deleteResultBitmap();
        return;
    }

    auto& entry = thumbnailCache_[key];
    if (entry.bitmap && entry.bitmap != result.bitmap) {
        DeleteObject(entry.bitmap);
    }
    entry.modifiedTime = result.modifiedTime;
    entry.bytes = result.bytes;
    entry.bitmap = result.bitmap;
    entry.unavailable = result.bitmap == nullptr;
    result.bitmap = nullptr;

    auto& slot = clipThumbnails_[result.clipIndex];
    slot.bitmap = entry.bitmap;
    slot.loading = false;
    slot.unavailable = entry.unavailable;

    invalidateClipRow(result.clipIndex);
}

size_t MainWindow::clipIndexFromListItem(UINT itemId) const {
    if (itemId == static_cast<UINT>(-1)) {
        return static_cast<size_t>(-1);
    }
    const size_t row = static_cast<size_t>(itemId);
    if (libraryViewMode_ == LibraryViewMode::Gallery) {
        const size_t index = row * kGalleryClipColumns;
        return index < clips_.size() ? index : static_cast<size_t>(-1);
    }
    return row < clips_.size() ? row : static_cast<size_t>(-1);
}

size_t MainWindow::clipIndexAtPoint(POINT point) const {
    if (!clipList_) {
        return static_cast<size_t>(-1);
    }

    const DWORD hit = static_cast<DWORD>(SendMessageW(
        clipList_, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y)));
    if (HIWORD(hit) != 0) {
        return static_cast<size_t>(-1);
    }

    const int row = LOWORD(hit);
    if (row < 0) {
        return static_cast<size_t>(-1);
    }
    if (libraryViewMode_ != LibraryViewMode::Gallery) {
        return clipIndexFromListItem(static_cast<UINT>(row));
    }

    RECT client{};
    GetClientRect(clipList_, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int columnWidth = std::max(1, width / kGalleryClipColumns);
    const int column = std::clamp(static_cast<int>(point.x / columnWidth), 0, kGalleryClipColumns - 1);
    const size_t index = static_cast<size_t>(row) * kGalleryClipColumns + static_cast<size_t>(column);
    return index < clips_.size() ? index : static_cast<size_t>(-1);
}

void MainWindow::updateGallerySelectionFromCursor() {
    if (!clipList_ || libraryViewMode_ != LibraryViewMode::Gallery || clips_.empty()) {
        return;
    }

    POINT point{};
    if (!GetCursorPos(&point)) {
        return;
    }
    ScreenToClient(clipList_, &point);

    RECT client{};
    GetClientRect(clipList_, &client);
    if (point.x < client.left || point.x >= client.right || point.y < client.top || point.y >= client.bottom) {
        return;
    }

    const size_t index = clipIndexAtPoint(point);
    if (index < clips_.size() && selectedClipIndex_ != index) {
        const size_t previousIndex = selectedClipIndex_;
        selectedClipIndex_ = index;
        invalidateClipRow(previousIndex);
        invalidateClipRow(selectedClipIndex_);
    }
}

void MainWindow::armClipDrag(POINT point) {
    clipDragArmed_ = false;
    clipDragPath_.clear();
    const size_t index = clipIndexAtPoint(point);
    if (index >= clips_.size()) {
        return;
    }

    if (selectedClipIndex_ != index) {
        const size_t previousIndex = selectedClipIndex_;
        selectedClipIndex_ = index;
        invalidateClipRow(previousIndex);
        invalidateClipRow(selectedClipIndex_);
    }
    SendMessageW(clipList_, LB_SETCURSEL,
        static_cast<WPARAM>(libraryViewMode_ == LibraryViewMode::Gallery ? index / kGalleryClipColumns : index), 0);
    setText(renameEdit_, clips_[index].path.stem().wstring());
    clipDragStartPoint_ = point;
    clipDragPath_ = clips_[index].path;
    clipDragArmed_ = true;
}

void MainWindow::beginClipDrag() {
    clipDragArmed_ = false;
    const std::filesystem::path path = std::move(clipDragPath_);
    clipDragPath_.clear();
    if (path.empty() || !std::filesystem::is_regular_file(path)) {
        setStatus(L"Clip is no longer available to drag");
        return;
    }
    Logger::instance().info(L"Starting Shell drag for clip: " + path.wstring());

    PIDLIST_ABSOLUTE absolutePidl = nullptr;
    HRESULT result = SHParseDisplayName(path.c_str(), nullptr, &absolutePidl, 0, nullptr);
    PIDLIST_ABSOLUTE parentPidl = nullptr;
    Microsoft::WRL::ComPtr<IDataObject> dataObject;
    if (SUCCEEDED(result) && absolutePidl) {
        parentPidl = ILCloneFull(absolutePidl);
        const PCUITEMID_CHILD childPidl = ILFindLastID(absolutePidl);
        if (parentPidl && childPidl && ILRemoveLastID(parentPidl)) {
            PCUITEMID_CHILD children[] = {childPidl};
            result = SHCreateDataObject(parentPidl, 1, children, nullptr, IID_IDataObject,
                reinterpret_cast<void**>(dataObject.GetAddressOf()));
        } else {
            result = E_FAIL;
        }
    }
    if (parentPidl) {
        CoTaskMemFree(parentPidl);
    }
    if (absolutePidl) {
        CoTaskMemFree(absolutePidl);
    }
    if (FAILED(result) || !dataObject) {
        Logger::instance().warning(L"Could not create Shell drag data for clip: " + path.wstring());
        setStatus(L"Could not start file drag");
        return;
    }

    auto* dropSource = new ClipFileDropSource();
    DWORD effect = DROPEFFECT_NONE;
    result = DoDragDrop(dataObject.Get(), dropSource, DROPEFFECT_COPY, &effect);
    dropSource->Release();
    if (result == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE) {
        setStatus(L"Dropped " + path.filename().wstring());
    } else if (FAILED(result)) {
        Logger::instance().warning(L"Shell file drag failed for " + path.wstring() + L"; HRESULT=" + std::to_wstring(result));
        setStatus(L"Could not start file drag");
    }
}

void MainWindow::invalidateClipRow(size_t clipIndex) {
    if (!clipList_ || clipIndex >= clips_.size()) {
        return;
    }

    const WPARAM row = libraryViewMode_ == LibraryViewMode::Gallery
        ? static_cast<WPARAM>(clipIndex / kGalleryClipColumns)
        : static_cast<WPARAM>(clipIndex);
    RECT rowRect{};
    if (SendMessageW(clipList_, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&rowRect)) != LB_ERR) {
        InvalidateRect(clipList_, &rowRect, FALSE);
    } else {
        InvalidateRect(clipList_, nullptr, FALSE);
    }
}

void MainWindow::refreshClips() {
    if (!clipList_) {
        return;
    }

    clipManager_.setDirectory(controller_.settings().clipDirectory);
    clips_ = clipManager_.listClips();
    selectedClipIndex_ = static_cast<size_t>(-1);
    loadClipThumbnails();
    ScopedRedrawLock redraw(clipList_);
    SendMessageW(clipList_, LB_RESETCONTENT, 0, 0);
    if (clips_.empty()) {
        SendMessageW(clipList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No clips yet"));
        setText(renameEdit_, L"");
        setStatus(L"No clips found");
        return;
    }

    if (libraryViewMode_ == LibraryViewMode::Gallery) {
        const size_t rows = (clips_.size() + kGalleryClipColumns - 1) / kGalleryClipColumns;
        for (size_t row = 0; row < rows; ++row) {
            const auto index = SendMessageW(clipList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L""));
            if (index != LB_ERR) {
                SendMessageW(clipList_, LB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(row));
            }
        }
    } else {
        for (const auto& clip : clips_) {
            std::wstring row = clip.favorite ? L"* " : L"  ";
            row += clip.path.filename().wstring();
            const std::wstring tagText = tagsToText(clip.tags);
            if (!tagText.empty()) {
                row += L" [" + tagText + L"]";
            }
            const auto index = SendMessageW(clipList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
            if (index != LB_ERR) {
                SendMessageW(clipList_, LB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(index));
            }
        }
    }
    Logger::instance().info(L"Library refreshed: " + std::to_wstring(clips_.size()) + L" clips in " + controller_.settings().clipDirectory.wstring());
    std::wstring status = std::to_wstring(clips_.size()) + (clips_.size() == 1 ? L" clip found" : L" clips found");
    const bool loadingPreviews = std::any_of(clipThumbnails_.begin(), clipThumbnails_.end(), [](const ClipThumbnail& thumbnail) {
        return thumbnail.loading;
    });
    if (loadingPreviews) {
        status += L"; loading previews";
    }
    setStatus(status);
}

void MainWindow::onClipSelectionChanged() {
    if (clipList_ && libraryViewMode_ == LibraryViewMode::List) {
        const auto selected = SendMessageW(clipList_, LB_GETCURSEL, 0, 0);
        if (selected != LB_ERR && selected >= 0) {
            selectedClipIndex_ = clipIndexFromListItem(static_cast<UINT>(selected));
        }
    } else if (clipList_ && selectedClipIndex_ >= clips_.size()) {
        const auto selected = SendMessageW(clipList_, LB_GETCURSEL, 0, 0);
        if (selected != LB_ERR && selected >= 0) {
            selectedClipIndex_ = clipIndexFromListItem(static_cast<UINT>(selected));
        }
    }
    const auto path = selectedClipPath();
    if (!path.empty()) {
        setText(renameEdit_, path.stem().wstring());
    } else {
        setText(renameEdit_, L"");
    }
}

void MainWindow::openSelectedClip() {
    const auto path = selectedClipPath();
    if (path.empty()) {
        setStatus(L"Select a clip to open");
        return;
    }

    if (shellExecuteSucceeded(ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL))) {
        Logger::instance().info(std::wstring(L"Opened clip from library: ") + path.wstring());
        setStatus(L"Opened " + path.filename().wstring());
    } else {
        Logger::instance().warning(std::wstring(L"Shell could not open clip from library: ") + path.wstring());
        setStatus(L"Clip could not be opened");
    }
}

void MainWindow::deleteSelectedClip() {
    const auto path = selectedClipPath();
    if (path.empty()) {
        setStatus(L"Select a clip to delete");
        return;
    }

    const std::wstring message =
        L"Delete " + path.filename().wstring() + L"?\n\nThis removes the clip file from disk.";
    if (MessageBoxW(window_, message.c_str(), L"Delete Clip", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        setStatus(L"Delete canceled");
        return;
    }

    if (clipManager_.removeClip(path)) {
        Logger::instance().info(std::wstring(L"Deleted clip from library: ") + path.wstring());
        refreshClips();
        setStatus(L"Clip deleted");
    } else {
        Logger::instance().warning(std::wstring(L"Clip delete failed from library: ") + path.wstring());
        setStatus(L"Clip could not be deleted");
    }
}

void MainWindow::renameSelectedClip() {
    const auto path = selectedClipPath();
    const auto newName = trimText(readText(renameEdit_));
    if (path.empty()) {
        setStatus(L"Select a clip to rename");
        return;
    }
    if (newName.empty()) {
        setStatus(L"Clip name cannot be empty");
        return;
    }

    if (clipManager_.renameClip(path, newName)) {
        Logger::instance().info(std::wstring(L"Renamed clip from library: ") + path.wstring() + L" -> " + newName);
        refreshClips();
        setStatus(L"Clip renamed");
    } else {
        Logger::instance().warning(std::wstring(L"Clip rename failed from library: ") + path.wstring() + L" -> " + newName);
        setStatus(L"Clip could not be renamed");
    }
}

void MainWindow::toggleSelectedFavorite() {
    const auto path = selectedClipPath();
    if (path.empty()) {
        setStatus(L"Select a clip to favorite");
        return;
    }
    const bool currentlyFavorite = std::filesystem::exists(path.parent_path() / (path.filename().wstring() + L".favorite"));
    if (clipManager_.setFavorite(path, !currentlyFavorite)) {
        Logger::instance().info(std::wstring(currentlyFavorite ? L"Removed favorite marker from clip: " : L"Marked clip as favorite: ") + path.wstring());
        refreshClips();
        setStatus(currentlyFavorite ? L"Favorite removed" : L"Clip favorited");
    } else {
        Logger::instance().warning(std::wstring(L"Favorite update failed for clip: ") + path.wstring());
        setStatus(L"Favorite could not be updated");
    }
}

std::filesystem::path MainWindow::selectedClipPath() const {
    if (!clipList_) {
        return {};
    }
    if (selectedClipIndex_ < clips_.size()) {
        return clips_[selectedClipIndex_].path;
    }
    const auto selected = SendMessageW(clipList_, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR || selected < 0) {
        return {};
    }
    const size_t index = clipIndexFromListItem(static_cast<UINT>(selected));
    if (index >= clips_.size()) {
        return {};
    }
    return clips_[index].path;
}

void MainWindow::handleHotkey(int id) {
    if (id == HotkeyService::kStartStopId) {
        ControllerAction action;
        action.kind = ControllerActionKind::ToggleRecording;
        if (queueControllerAction(std::move(action), L"Recorder is busy; hotkey ignored")) {
            setStatus(controller_.stats().recording ? L"Stopping recording..." : L"Starting recording...");
        }
    } else if (id == HotkeyService::kSaveReplayId) {
        ControllerAction action;
        action.kind = ControllerActionKind::SaveReplay;
        const auto now = SteadyClock::now();
        if (killClipTagDeadline_ != SteadyClock::time_point::min()) {
            if (now <= killClipTagDeadline_) {
                action.replayTag = L"Kill clip";
            } else {
                killClipTagDeadline_ = SteadyClock::time_point::min();
            }
        }
        const bool taggingKillClip = !action.replayTag.empty();
        if (queueControllerAction(std::move(action), L"Recorder is busy; replay hotkey ignored")) {
            if (taggingKillClip) {
                killClipTagDeadline_ = SteadyClock::time_point::min();
            }
            setStatus(taggingKillClip ? L"Saving kill clip..." : L"Saving replay...");
        }
    }
}

void MainWindow::updateGameIntegrations() {
    settings_ = controller_.settings();
    const bool leagueEnabled =
        settings_.replay.enabled &&
        settings_.gameIntegrations.leagueOfLegendsKillReminder;
    if (leagueEnabled) {
        leagueIntegration_.start();
    } else {
        leagueIntegration_.stop();
    }
}

void MainWindow::stopGameIntegrations() {
    leagueIntegration_.stop();
}

void MainWindow::handleLeagueKillDetected() {
    settings_ = controller_.settings();
    if (!settings_.replay.enabled || !settings_.gameIntegrations.leagueOfLegendsKillReminder) {
        return;
    }

    killClipTagDeadline_ = SteadyClock::now() + std::chrono::seconds(6);
    Logger::instance().info(L"League of Legends kill reminder triggered; replay saves are taggable for 6 seconds");
    playActionIndicator(MB_ICONASTERISK);
    setStatus(L"Kill detected; press replay hotkey within 6 seconds to tag it");
}

bool MainWindow::handleShortcut(MSG& message) {
    if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) {
        return false;
    }

    HWND focused = GetFocus();
    if (focused == recordHotkey_ || focused == replayHotkey_) {
        if (isModifierKey(message.wParam)) {
            return true;
        }

        uint32_t modifiers = 0;
        uint32_t virtualKey = 0;
        if (message.wParam != VK_BACK && message.wParam != VK_DELETE) {
            modifiers = currentModifierState();
            virtualKey = static_cast<uint32_t>(message.wParam);
            if (!isSafeGlobalHotkey(modifiers, virtualKey)) {
                setStatus(L"Hotkeys for normal keys need Ctrl or Alt");
                return true;
            }
        }

        if (focused == recordHotkey_) {
            recordHotkeyModifiers_ = modifiers;
            recordHotkeyVirtualKey_ = virtualKey;
        } else {
            replayHotkeyModifiers_ = modifiers;
            replayHotkeyVirtualKey_ = virtualKey;
        }
        setText(focused, hotkeyDisplay(modifiers, virtualKey));
        markSettingsDirty();
        return true;
    }

    if (page_ != Page::Library) {
        return false;
    }

    if (message.wParam == VK_F2 && renameEdit_) {
        const auto path = selectedClipPath();
        if (!path.empty()) {
            setText(renameEdit_, path.stem().wstring());
        }
        SetFocus(renameEdit_);
        SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
        return true;
    }

    if (message.wParam == VK_RETURN && renameEdit_ && GetFocus() == renameEdit_) {
        renameSelectedClip();
        return true;
    }

    if (GetFocus() != renameEdit_) {
        if (message.wParam == VK_DELETE) {
            deleteSelectedClip();
            return true;
        }
        if (message.wParam == VK_F5 || (isControlDown() && message.wParam == L'R')) {
            refreshClips();
            return true;
        }
        if (isControlDown() && message.wParam == L'O') {
            openSelectedClip();
            return true;
        }
    }

    return false;
}

void MainWindow::addTrayIcon() {
    if (trayVisible_) {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(window_, GCLP_HICON));
    if (!data.hIcon) {
        data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    lstrcpynW(data.szTip, L"Backtrack", static_cast<int>(_countof(data.szTip)));
    trayVisible_ = Shell_NotifyIconW(NIM_ADD, &data) == TRUE;
}

void MainWindow::removeTrayIcon() {
    if (!trayVisible_) {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    trayVisible_ = false;
}

void MainWindow::restoreFromTray() {
    removeTrayIcon();
    ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOWNORMAL);
    BringWindowToTop(window_);
    SetForegroundWindow(window_);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
}

void MainWindow::showTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kTrayOpenId, L"Open Backtrack");
    AppendMenuW(menu, MF_STRING, kTrayExitId, L"Exit");

    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
    DestroyMenu(menu);

    if (command == kTrayOpenId) {
        restoreFromTray();
    } else if (command == kTrayExitId) {
        exitFromTray_ = true;
        removeTrayIcon();
        DestroyWindow(window_);
    }
}

void MainWindow::drawButtonItem(const DRAWITEMSTRUCT& item) {
    const int controlId = static_cast<int>(item.CtlID);
    if (isSoundSeparationMuteButtonId(controlId)) {
        const size_t index = soundSeparationIndexFromButtonId(controlId, kSoundSeparationMuteButtonBaseId);
        const bool muted = index < settings_.soundSeparationApps.size() && settings_.soundSeparationApps[index].muted;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        RECT rect = item.rcItem;
        FillRect(item.hDC, &rect, pressed ? selectionBrush_ : controlBrush_);

        HGDIOBJ oldOutline = SelectObject(item.hDC, outlinePen_);
        HGDIOBJ oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
        Rectangle(item.hDC, rect.left, rect.top, rect.right, rect.bottom);
        SelectObject(item.hDC, oldBrush);
        SelectObject(item.hDC, oldOutline);

        const COLORREF iconColor = muted ? RGB(232, 72, 72) : (disabled ? kMutedText : kText);
        HPEN iconPen = CreatePen(PS_SOLID, 2, iconColor);
        HBRUSH iconBrush = CreateSolidBrush(iconColor);
        HGDIOBJ oldPen = SelectObject(item.hDC, iconPen);
        HGDIOBJ oldIconBrush = SelectObject(item.hDC, iconBrush);

        const int dx = pressed ? 1 : 0;
        const int dy = pressed ? 1 : 0;
        const int cx = (rect.left + rect.right) / 2 + dx;
        const int top = rect.top + 4 + dy;
        RoundRect(item.hDC, cx - 5, top, cx + 5, top + 13, 6, 6);
        SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
        Arc(item.hDC, cx - 10, top + 6, cx + 10, top + 21, cx - 9, top + 13, cx + 9, top + 13);
        MoveToEx(item.hDC, cx, top + 19, nullptr);
        LineTo(item.hDC, cx, top + 24);
        MoveToEx(item.hDC, cx - 6, top + 24, nullptr);
        LineTo(item.hDC, cx + 6, top + 24);
        if (muted) {
            MoveToEx(item.hDC, cx - 10, top + 2, nullptr);
            LineTo(item.hDC, cx + 10, top + 22);
        }

        SelectObject(item.hDC, oldIconBrush);
        SelectObject(item.hDC, oldPen);
        DeleteObject(iconBrush);
        DeleteObject(iconPen);

        if ((item.itemState & ODS_FOCUS) != 0) {
            DrawFocusRect(item.hDC, &item.rcItem);
        }
        return;
    }

    const bool selectedTab = controlId >= kTabButtonBaseId &&
                             controlId < kTabButtonBaseId + kTabCount &&
                             controlId == kTabButtonBaseId + static_cast<int>(page_);
    const bool selectedLibraryView =
        (controlId == kClipListViewButtonId && libraryViewMode_ == LibraryViewMode::List) ||
        (controlId == kClipGalleryViewButtonId && libraryViewMode_ == LibraryViewMode::Gallery);
    const bool selectedSettingsCategory =
        page_ == Page::Settings &&
        controlId >= kSettingsCategoryButtonBaseId &&
        controlId < kSettingsCategoryButtonBaseId + kSettingsCategoryCount &&
        controlId == kSettingsCategoryButtonBaseId + static_cast<int>(settingsCategory_);
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool selected = selectedTab || selectedLibraryView || selectedSettingsCategory;
    const COLORREF textColor = selected ? kText : (disabled ? kMutedText : kText);

    HBRUSH brush = selected || pressed ? selectionBrush_ : controlBrush_;
    FillRect(item.hDC, &item.rcItem, brush);

    HGDIOBJ oldPen = SelectObject(item.hDC, outlinePen_);
    HGDIOBJ oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);

    wchar_t textBuffer[256]{};
    GetWindowTextW(item.hwndItem, textBuffer, static_cast<int>(_countof(textBuffer)));

    RECT textRect = item.rcItem;
    textRect.left += 8;
    textRect.right -= 8;
    if (pressed) {
        OffsetRect(&textRect, 1, 1);
    }
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, textColor);
    DrawTextW(item.hDC, textBuffer, -1, &textRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);

    if ((item.itemState & ODS_FOCUS) != 0) {
        DrawFocusRect(item.hDC, &item.rcItem);
    }
}

void MainWindow::drawClipItem(const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1)) {
        return;
    }

    const bool emptyState = clips_.empty();
    if (libraryViewMode_ == LibraryViewMode::Gallery && !emptyState) {
        FillRect(item.hDC, &item.rcItem, editBrush_);

        const int rowWidth = std::max(1, static_cast<int>(item.rcItem.right - item.rcItem.left));
        const int gap = 8;
        const int tileWidth = std::max(1, (rowWidth - gap * (kGalleryClipColumns + 1)) / kGalleryClipColumns);
        for (int column = 0; column < kGalleryClipColumns; ++column) {
            const size_t clipIndex = static_cast<size_t>(item.itemID) * kGalleryClipColumns + static_cast<size_t>(column);
            if (clipIndex >= clips_.size()) {
                continue;
            }

            const auto& clip = clips_[clipIndex];
            RECT tile{
                item.rcItem.left + gap + column * (tileWidth + gap),
                item.rcItem.top + gap,
                item.rcItem.left + gap + column * (tileWidth + gap) + tileWidth,
                item.rcItem.bottom - gap,
            };
            const bool selected = selectedClipIndex_ == clipIndex || ((item.itemState & ODS_SELECTED) != 0 && selectedClipIndex_ >= clips_.size() && column == 0);
            const COLORREF text = clip.favorite ? kFavoriteText : kText;

            HBRUSH tileBrush = clip.favorite ? favoriteBrush_ : (selected ? selectionBrush_ : controlBrush_);
            FillRect(item.hDC, &tile, tileBrush);

            HGDIOBJ oldPen = SelectObject(item.hDC, selected ? selectedOutlinePen_ : outlinePen_);
            HGDIOBJ oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
            Rectangle(item.hDC, tile.left, tile.top, tile.right, tile.bottom);
            SelectObject(item.hDC, oldBrush);
            SelectObject(item.hDC, oldPen);

            RECT preview = tile;
            preview.left += 8;
            preview.right -= 8;
            preview.top += 8;
            preview.bottom = preview.top + std::max(64, static_cast<int>((preview.right - preview.left) * 9 / 16));
            preview.bottom = std::min(preview.bottom, tile.bottom - 46);

            FillRect(item.hDC, &preview, editBrush_);
            const ClipThumbnail* thumbnail = clipIndex < clipThumbnails_.size() ? &clipThumbnails_[clipIndex] : nullptr;
            if (thumbnail && thumbnail->bitmap) {
                drawBitmapFit(item.hDC, thumbnail->bitmap, preview);
            } else {
                SetBkMode(item.hDC, TRANSPARENT);
                SetTextColor(item.hDC, kMutedText);
                const wchar_t* previewText = thumbnail && thumbnail->loading ? L"Loading preview..." : L"Preview unavailable";
                DrawTextW(item.hDC, previewText, -1, &preview, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            RECT nameRect = tile;
            nameRect.left += 8;
            nameRect.right -= 8;
            nameRect.top = preview.bottom + 8;
            nameRect.bottom = nameRect.top + 22;
            std::wstring name = clip.favorite ? L"* " + clip.path.filename().wstring() : clip.path.filename().wstring();
            SetBkMode(item.hDC, TRANSPARENT);
            SetTextColor(item.hDC, text);
            DrawTextW(item.hDC, name.c_str(), -1, &nameRect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

            RECT metaRect = tile;
            metaRect.left += 8;
            metaRect.right -= 8;
            metaRect.top = nameRect.bottom + 2;
            metaRect.bottom = tile.bottom - 6;
            std::wstring meta = durationToText(clip.duration100ns) + L"  " + bytesToText(clip.bytes);
            const std::wstring tagText = tagsToText(clip.tags);
            if (!tagText.empty()) {
                meta += L"  " + tagText;
            }
            SetTextColor(item.hDC, clip.favorite ? kFavoriteText : kMutedText);
            DrawTextW(item.hDC, meta.c_str(), -1, &metaRect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
        }

        if ((item.itemState & ODS_FOCUS) != 0) {
            DrawFocusRect(item.hDC, &item.rcItem);
        }
        return;
    }

    const size_t clipIndex = clipIndexFromListItem(item.itemID);
    const bool favorite = !emptyState && clipIndex < clips_.size() && clips_[clipIndex].favorite;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const COLORREF text = emptyState ? kMutedText : (favorite ? kFavoriteText : kText);

    HBRUSH brush = favorite ? favoriteBrush_ : (selected ? selectionBrush_ : editBrush_);
    FillRect(item.hDC, &item.rcItem, brush);

    const ClipInfo* clip = !emptyState && clipIndex < clips_.size() ? &clips_[clipIndex] : nullptr;
    std::wstring name = emptyState ? L"No clips yet" : (clip ? clip->path.filename().wstring() : L"");
    if (favorite) {
        name = L"* " + name;
    }
    if (clip) {
        const std::wstring tagText = tagsToText(clip->tags);
        if (!tagText.empty()) {
            name += L" [" + tagText + L"]";
        }
    }
    const std::wstring sizeText = clip ? bytesToText(clip->bytes) : L"";
    const std::wstring durationText = clip ? durationToText(clip->duration100ns) : L"";

    RECT sizeRect = item.rcItem;
    sizeRect.left = sizeRect.right - 98;
    sizeRect.right -= 8;
    RECT durationRect = item.rcItem;
    durationRect.left = sizeRect.left - 72;
    durationRect.right = sizeRect.left - 12;

    RECT textRect = item.rcItem;
    textRect.left += 8;
    textRect.right = durationRect.left - 10;
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, text);
    DrawTextW(item.hDC, name.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    DrawTextW(item.hDC, durationText.c_str(), -1, &durationRect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);
    DrawTextW(item.hDC, sizeText.c_str(), -1, &sizeRect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);

    if ((item.itemState & ODS_FOCUS) != 0) {
        DrawFocusRect(item.hDC, &item.rcItem);
    }
}

void MainWindow::applyDarkTheme(HWND control) {
    if (!control) {
        return;
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(control, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
}

void MainWindow::setStatus(const std::wstring& status) {
    if (statusText_ == status && (!hoveredHelpControl_ || displayedStatusText_ == status)) {
        return;
    }
    statusText_ = status;
    if (!hoveredHelpControl_ && !buildingPage_) {
        applyStatusText(statusText_);
    }
}

} // namespace backtrack
