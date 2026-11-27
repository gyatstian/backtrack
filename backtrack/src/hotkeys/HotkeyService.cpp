#include "hotkeys/HotkeyService.h"

#include "core/LogContext.h"
#include "core/Logger.h"
#include "settings/SettingsStore.h"

namespace backtrack {

namespace {

constexpr LPARAM kHookHotkeyLParam = 0x4254484b;
constexpr ULONGLONG kRegisteredDuplicateWindowMs = 250;

std::wstring hotkeyName(uint32_t modifiers, uint32_t virtualKey) {
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

    wchar_t name[64]{};
    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    LONG lParam = static_cast<LONG>(scanCode << 16);
    switch (virtualKey) {
    case VK_CANCEL:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_END:
    case VK_HOME:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
    case VK_SNAPSHOT:
    case VK_INSERT:
    case VK_DELETE:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
    case VK_DIVIDE:
    case VK_NUMLOCK:
    case VK_RCONTROL:
    case VK_RMENU:
        lParam |= (1 << 24);
        break;
    default:
        break;
    }
    if (GetKeyNameTextW(lParam, name, static_cast<int>(_countof(name))) > 0) {
        value += name;
    } else {
        value += L"VK " + std::to_wstring(virtualKey);
    }
    return value;
}

std::wstring hotkeyFailureMessage(const wchar_t* label, uint32_t modifiers, uint32_t virtualKey, DWORD error) {
    std::wstring message = std::wstring(L"Could not register ") + label + L" hotkey (" + hotkeyName(modifiers, virtualKey) + L")";
    if (error == ERROR_HOTKEY_ALREADY_REGISTERED) {
        message += L": already registered by another app";
    } else if (error != ERROR_SUCCESS) {
        message += L": Windows error " + std::to_wstring(error);
    }
    return message;
}

std::wstring keyboardHookFailureMessage(DWORD error) {
    std::wstring message = L"Could not install fullscreen hotkey hook";
    if (error != ERROR_SUCCESS) {
        message += L": Windows error " + std::to_wstring(error);
    }
    return message;
}

bool isModifierVirtualKey(uint32_t virtualKey) {
    switch (virtualKey) {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return true;
    default:
        return false;
    }
}

bool isKeyDownNow(int virtualKey) {
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

} // namespace

HotkeyService* HotkeyService::activeService_ = nullptr;

HotkeyService::~HotkeyService() {
    if (activeService_ == this) {
        activeService_ = nullptr;
    }
}

bool HotkeyService::registerHotkeys(HWND window, const HotkeySettings& settings) {
    unregisterHotkeys(window);
    lastErrorMessage_.clear();
    window_ = window;
    settings_ = settings;
    seedModifierState();
    startStopPressed_ = false;
    saveReplayPressed_ = false;
    startStopHookTick_ = 0;
    saveReplayHookTick_ = 0;

    DWORD hookError = ERROR_SUCCESS;
    bool hookOk = true;
    const bool wantsHook = settings.startStopVirtualKey != 0 || settings.saveReplayVirtualKey != 0;
    if (wantsHook) {
        // Installed unconditionally, not as a RegisterHotKey fallback: a
        // fullscreen game or anti-cheat can break the low-level hook chain, so
        // RegisterHotKey is not reliably the redundant one of the two.
        activeService_ = this;
        keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, GetModuleHandleW(nullptr), 0);
        if (!keyboardHook_) {
            hookError = GetLastError();
            hookOk = false;
            if (activeService_ == this) {
                activeService_ = nullptr;
            }
            Logger::instance().warning(L"hotkeys", keyboardHookFailureMessage(hookError));
        } else {
            LogContext::breadcrumb(L"fullscreen hook armed");
        }
    }

    auto registerOne = [window](int id, uint32_t modifiers, uint32_t virtualKey, DWORD& error) {
        if (virtualKey == 0) {
            return true;
        }
        if (!RegisterHotKey(window, id, modifiers | MOD_NOREPEAT, virtualKey)) {
            error = GetLastError();
            return false;
        }
        return true;
    };
    auto appendError = [this](const std::wstring& message) {
        if (!lastErrorMessage_.empty()) {
            lastErrorMessage_ += L"; ";
        }
        lastErrorMessage_ += message;
    };
    DWORD startStopError = ERROR_SUCCESS;
    const bool startStop = registerOne(kStartStopId, settings.startStopModifiers, settings.startStopVirtualKey, startStopError);
    DWORD saveReplayError = ERROR_SUCCESS;
    const bool saveReplay = registerOne(kSaveReplayId, settings.saveReplayModifiers, settings.saveReplayVirtualKey, saveReplayError);

    if (!startStop) {
        const std::wstring message = hotkeyFailureMessage(L"start/stop", settings.startStopModifiers, settings.startStopVirtualKey, startStopError);
        Logger::instance().warning(L"hotkeys", hookOk ? message + L"; fullscreen hook remains active" : message);
        if (!hookOk) {
            appendError(message);
        }
    }
    if (!saveReplay) {
        const std::wstring message = hotkeyFailureMessage(L"save replay", settings.saveReplayModifiers, settings.saveReplayVirtualKey, saveReplayError);
        Logger::instance().warning(L"hotkeys", hookOk ? message + L"; fullscreen hook remains active" : message);
        if (!hookOk) {
            appendError(message);
        }
    }
    if ((!startStop || !saveReplay) && !hookOk) {
        appendError(keyboardHookFailureMessage(hookError));
        unregisterHotkeys(window);
        return false;
    }
    Logger::instance().info(L"hotkeys",
        L"Hotkeys registered: start/stop " +
        hotkeyName(settings.startStopModifiers, settings.startStopVirtualKey) +
        L", save replay " +
        hotkeyName(settings.saveReplayModifiers, settings.saveReplayVirtualKey) +
        (wantsHook ? L"; fullscreen hook active" : L"; no fullscreen hook"));
    return true;
}

void HotkeyService::unregisterHotkeys(HWND window) {
    if (keyboardHook_) {
        LogContext::breadcrumb(L"fullscreen hook released");
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    if (activeService_ == this) {
        activeService_ = nullptr;
    }
    UnregisterHotKey(window, kStartStopId);
    UnregisterHotKey(window, kSaveReplayId);
    window_ = nullptr;
    settings_ = HotkeySettings{};
    leftCtrlDown_ = false;
    rightCtrlDown_ = false;
    leftAltDown_ = false;
    rightAltDown_ = false;
    leftShiftDown_ = false;
    rightShiftDown_ = false;
    startStopPressed_ = false;
    saveReplayPressed_ = false;
    startStopHookTick_ = 0;
    saveReplayHookTick_ = 0;
    Logger::instance().info(L"hotkeys", L"Hotkeys unregistered; fullscreen hook released");
}

bool HotkeyService::shouldHandleHotkeyMessage(int id, LPARAM lParam) const {
    if (lParam == kHookHotkeyLParam) {
        return true;
    }

    const ULONGLONG lastHookTick =
        id == kStartStopId ? startStopHookTick_ :
        id == kSaveReplayId ? saveReplayHookTick_ :
        0;
    return lastHookTick == 0 || GetTickCount64() - lastHookTick > kRegisteredDuplicateWindowMs;
}

LRESULT CALLBACK HotkeyService::keyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {
    HotkeyService* service = activeService_;
    if (code == HC_ACTION && service) {
        service->handleKeyboardEvent(wParam, *reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam));
    }
    return CallNextHookEx(service ? service->keyboardHook_ : nullptr, code, wParam, lParam);
}

void HotkeyService::handleKeyboardEvent(WPARAM message, const KBDLLHOOKSTRUCT& event) {
    const bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!keyDown && !keyUp) {
        return;
    }

    const uint32_t virtualKey = event.vkCode;
    updateModifierState(event, keyDown);
    if (keyUp) {
        if (virtualKey == settings_.startStopVirtualKey) {
            startStopPressed_ = false;
        }
        if (virtualKey == settings_.saveReplayVirtualKey) {
            saveReplayPressed_ = false;
        }
        return;
    }

    if (isModifierVirtualKey(virtualKey)) {
        return;
    }

    const uint32_t modifiers = currentHookModifiers();
    if (matchesHotkey(modifiers, virtualKey, settings_.startStopModifiers, settings_.startStopVirtualKey)) {
        if (!startStopPressed_) {
            startStopPressed_ = postHookHotkey(kStartStopId);
        }
    }
    if (matchesHotkey(modifiers, virtualKey, settings_.saveReplayModifiers, settings_.saveReplayVirtualKey)) {
        if (!saveReplayPressed_) {
            saveReplayPressed_ = postHookHotkey(kSaveReplayId);
        }
    }
}

void HotkeyService::seedModifierState() {
    leftCtrlDown_ = isKeyDownNow(VK_LCONTROL);
    rightCtrlDown_ = isKeyDownNow(VK_RCONTROL);
    leftAltDown_ = isKeyDownNow(VK_LMENU);
    rightAltDown_ = isKeyDownNow(VK_RMENU);
    leftShiftDown_ = isKeyDownNow(VK_LSHIFT);
    rightShiftDown_ = isKeyDownNow(VK_RSHIFT);
}

void HotkeyService::updateModifierState(const KBDLLHOOKSTRUCT& event, bool down) {
    uint32_t virtualKey = event.vkCode;
    if (virtualKey == VK_CONTROL) {
        virtualKey = (event.flags & LLKHF_EXTENDED) ? VK_RCONTROL : VK_LCONTROL;
    } else if (virtualKey == VK_MENU) {
        virtualKey = (event.flags & LLKHF_EXTENDED) ? VK_RMENU : VK_LMENU;
    } else if (virtualKey == VK_SHIFT) {
        // a generic shift without a usable scan code cannot identify a side.
        virtualKey = MapVirtualKeyW(event.scanCode, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? VK_RSHIFT : VK_LSHIFT;
    }

    switch (virtualKey) {
    case VK_LCONTROL: leftCtrlDown_ = down; break;
    case VK_RCONTROL: rightCtrlDown_ = down; break;
    case VK_LMENU: leftAltDown_ = down; break;
    case VK_RMENU: rightAltDown_ = down; break;
    case VK_LSHIFT: leftShiftDown_ = down; break;
    case VK_RSHIFT: rightShiftDown_ = down; break;
    }
}

uint32_t HotkeyService::currentHookModifiers() const {
    uint32_t modifiers = 0;
    if (leftCtrlDown_ || rightCtrlDown_) {
        modifiers |= MOD_CONTROL;
    }
    if (leftAltDown_ || rightAltDown_) {
        modifiers |= MOD_ALT;
    }
    if (leftShiftDown_ || rightShiftDown_) {
        modifiers |= MOD_SHIFT;
    }
    return modifiers;
}

bool HotkeyService::matchesHotkey(uint32_t modifiers, uint32_t virtualKey, uint32_t hotkeyModifiers, uint32_t hotkeyVirtualKey) const {
    return hotkeyVirtualKey != 0 &&
           virtualKey == hotkeyVirtualKey &&
           modifiers == supportedHotkeyModifiers(hotkeyModifiers);
}

bool HotkeyService::postHookHotkey(int id) {
    if (!window_) {
        return false;
    }
    if (!PostMessageW(window_, WM_HOTKEY, static_cast<WPARAM>(id), kHookHotkeyLParam)) {
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    if (id == kStartStopId) {
        startStopHookTick_ = now;
    } else if (id == kSaveReplayId) {
        saveReplayHookTick_ = now;
    }
    return true;
}

} // namespace backtrack
