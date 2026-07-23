#include "hotkeys/HotkeyService.h"

#include "core/Logger.h"

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

bool isControlVirtualKey(uint32_t virtualKey) {
    return virtualKey == VK_CONTROL || virtualKey == VK_LCONTROL || virtualKey == VK_RCONTROL;
}

bool isAltVirtualKey(uint32_t virtualKey) {
    return virtualKey == VK_MENU || virtualKey == VK_LMENU || virtualKey == VK_RMENU;
}

bool isShiftVirtualKey(uint32_t virtualKey) {
    return virtualKey == VK_SHIFT || virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT;
}

bool isModifierVirtualKey(uint32_t virtualKey) {
    return isControlVirtualKey(virtualKey) || isAltVirtualKey(virtualKey) || isShiftVirtualKey(virtualKey);
}

bool isKeyDownNow(int virtualKey) {
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

uint32_t supportedHookModifiers(uint32_t modifiers) {
    return modifiers & (MOD_ALT | MOD_CONTROL | MOD_SHIFT);
}

} // namespace

HotkeyService* HotkeyService::activeService_ = nullptr;

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
        activeService_ = this;
        keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, GetModuleHandleW(nullptr), 0);
        if (!keyboardHook_) {
            hookError = GetLastError();
            hookOk = false;
            if (activeService_ == this) {
                activeService_ = nullptr;
            }
            Logger::instance().warning(L"hotkeys", keyboardHookFailureMessage(hookError));
        }
    }

    DWORD startStopError = ERROR_SUCCESS;
    const BOOL startStop = settings.startStopVirtualKey == 0
        ? TRUE
        : RegisterHotKey(
              window,
              kStartStopId,
              settings.startStopModifiers | MOD_NOREPEAT,
              settings.startStopVirtualKey);
    if (!startStop) {
        startStopError = GetLastError();
    }

    DWORD saveReplayError = ERROR_SUCCESS;
    const BOOL saveReplay = settings.saveReplayVirtualKey == 0
        ? TRUE
        : RegisterHotKey(
              window,
              kSaveReplayId,
              settings.saveReplayModifiers | MOD_NOREPEAT,
              settings.saveReplayVirtualKey);
    if (!saveReplay) {
        saveReplayError = GetLastError();
    }

    if (!startStop) {
        const std::wstring message = hotkeyFailureMessage(L"start/stop", settings.startStopModifiers, settings.startStopVirtualKey, startStopError);
        Logger::instance().warning(L"hotkeys", hookOk ? message + L"; fullscreen hook remains active" : message);
        if (!hookOk) {
            lastErrorMessage_ = message;
        }
    }
    if (!saveReplay) {
        const std::wstring message = hotkeyFailureMessage(L"save replay", settings.saveReplayModifiers, settings.saveReplayVirtualKey, saveReplayError);
        Logger::instance().warning(L"hotkeys", hookOk ? message + L"; fullscreen hook remains active" : message);
        if (!hookOk) {
            if (!lastErrorMessage_.empty()) {
                lastErrorMessage_ += L"; ";
            }
            lastErrorMessage_ += message;
        }
    }
    if ((!startStop || !saveReplay) && !hookOk) {
        if (!lastErrorMessage_.empty()) {
            lastErrorMessage_ += L"; ";
        }
        lastErrorMessage_ += keyboardHookFailureMessage(hookError);
        unregisterHotkeys(window);
        return false;
    }
    return true;
}

void HotkeyService::unregisterHotkeys(HWND window) {
    if (keyboardHook_) {
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
    ctrlDown_ = false;
    altDown_ = false;
    shiftDown_ = false;
    startStopPressed_ = false;
    saveReplayPressed_ = false;
    startStopHookTick_ = 0;
    saveReplayHookTick_ = 0;
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
    updateModifierState(virtualKey, keyDown);
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
    ctrlDown_ = isKeyDownNow(VK_CONTROL) || isKeyDownNow(VK_LCONTROL) || isKeyDownNow(VK_RCONTROL);
    altDown_ = isKeyDownNow(VK_MENU) || isKeyDownNow(VK_LMENU) || isKeyDownNow(VK_RMENU);
    shiftDown_ = isKeyDownNow(VK_SHIFT) || isKeyDownNow(VK_LSHIFT) || isKeyDownNow(VK_RSHIFT);
}

void HotkeyService::updateModifierState(uint32_t virtualKey, bool down) {
    if (isControlVirtualKey(virtualKey)) {
        ctrlDown_ = down;
    } else if (isAltVirtualKey(virtualKey)) {
        altDown_ = down;
    } else if (isShiftVirtualKey(virtualKey)) {
        shiftDown_ = down;
    }
}

uint32_t HotkeyService::currentHookModifiers() const {
    uint32_t modifiers = 0;
    if (ctrlDown_) {
        modifiers |= MOD_CONTROL;
    }
    if (altDown_) {
        modifiers |= MOD_ALT;
    }
    if (shiftDown_) {
        modifiers |= MOD_SHIFT;
    }
    return modifiers;
}

bool HotkeyService::matchesHotkey(uint32_t modifiers, uint32_t virtualKey, uint32_t hotkeyModifiers, uint32_t hotkeyVirtualKey) const {
    return hotkeyVirtualKey != 0 &&
           virtualKey == hotkeyVirtualKey &&
           modifiers == supportedHookModifiers(hotkeyModifiers);
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
