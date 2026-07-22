#include "ui/UiHelpers.h"

#include "core/Logger.h"
#include "core/Types.h"

#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <sstream>
#include <vector>

namespace backtrack {

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

void redrawWindowAndChildren(HWND window, bool updateNow) {
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

ScopedRedrawLock::ScopedRedrawLock(HWND window)
    : window_(window) {
    if (window_) {
        SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
    }
}

ScopedRedrawLock::~ScopedRedrawLock() {
    if (window_) {
        SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
        redrawWindowAndChildren(window_);
    }
}

} // namespace backtrack