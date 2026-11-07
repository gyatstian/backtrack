#pragma once

#include "core/Types.h"

#include <Windows.h>

#include <string>

namespace backtrack {

class HotkeyService {
public:
    static constexpr int kStartStopId = 1001;
    static constexpr int kSaveReplayId = 1002;

    HotkeyService() = default;
    // The hook proc dereferences activeService_ on every keystroke, so a
    // destroyed service must never leave the static pointing at it.
    ~HotkeyService();

    bool registerHotkeys(HWND window, const HotkeySettings& settings);
    void unregisterHotkeys(HWND window);
    bool shouldHandleHotkeyMessage(int id, LPARAM lParam) const;
    const std::wstring& lastErrorMessage() const { return lastErrorMessage_; }

private:
    friend struct HotkeyServiceTestAccess;

    static LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam, LPARAM lParam);

    void handleKeyboardEvent(WPARAM message, const KBDLLHOOKSTRUCT& event);
    void seedModifierState();
    void updateModifierState(const KBDLLHOOKSTRUCT& event, bool down);
    uint32_t currentHookModifiers() const;
    bool matchesHotkey(uint32_t modifiers, uint32_t virtualKey, uint32_t hotkeyModifiers, uint32_t hotkeyVirtualKey) const;
    bool postHookHotkey(int id);

    static HotkeyService* activeService_;

    HWND window_ = nullptr;
    HHOOK keyboardHook_ = nullptr;
    HotkeySettings settings_{};
    bool leftCtrlDown_ = false;
    bool rightCtrlDown_ = false;
    bool leftAltDown_ = false;
    bool rightAltDown_ = false;
    bool leftShiftDown_ = false;
    bool rightShiftDown_ = false;
    bool startStopPressed_ = false;
    bool saveReplayPressed_ = false;
    ULONGLONG startStopHookTick_ = 0;
    ULONGLONG saveReplayHookTick_ = 0;
    std::wstring lastErrorMessage_;
};

} // namespace backtrack
