#include "hotkeys/HotkeyService.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace backtrack {

std::filesystem::path defaultClipDirectory() {
    return {};
}

struct HotkeyServiceTestAccess {
    static void event(HotkeyService& service, WPARAM message, const KBDLLHOOKSTRUCT& event) {
        service.handleKeyboardEvent(message, event);
    }
    static uint32_t modifiers(const HotkeyService& service) {
        return service.currentHookModifiers();
    }
    static bool matches(const HotkeyService& service, uint32_t key, uint32_t modifiers, uint32_t hotkey) {
        return service.matchesHotkey(service.currentHookModifiers(), key, modifiers, hotkey);
    }
    static void markPressed(HotkeyService& service) {
        service.settings_.startStopVirtualKey = VK_F9;
        service.settings_.saveReplayVirtualKey = VK_F10;
        service.startStopPressed_ = true;
        service.saveReplayPressed_ = true;
    }
    static bool startStopPressed(const HotkeyService& service) { return service.startStopPressed_; }
    static bool saveReplayPressed(const HotkeyService& service) { return service.saveReplayPressed_; }
};

} // namespace backtrack

namespace {

using backtrack::HotkeyService;
using backtrack::HotkeyServiceTestAccess;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Modifier {
    const char* name;
    DWORD genericKey;
    DWORD leftKey;
    DWORD rightKey;
    DWORD leftScan;
    DWORD rightScan;
    DWORD rightFlags;
    uint32_t mask;
};

constexpr Modifier kModifiers[] = {
    {"Ctrl", VK_CONTROL, VK_LCONTROL, VK_RCONTROL, 0x1d, 0x1d, LLKHF_EXTENDED, MOD_CONTROL},
    {"Alt", VK_MENU, VK_LMENU, VK_RMENU, 0x38, 0x38, LLKHF_EXTENDED, MOD_ALT},
    {"Shift", VK_SHIFT, VK_LSHIFT, VK_RSHIFT, 0x2a, 0x36, 0, MOD_SHIFT},
};

KBDLLHOOKSTRUCT modifierEvent(const Modifier& modifier, bool left, bool generic, bool injected) {
    KBDLLHOOKSTRUCT event{};
    event.vkCode = generic ? modifier.genericKey : left ? modifier.leftKey : modifier.rightKey;
    event.scanCode = left ? modifier.leftScan : modifier.rightScan;
    event.flags = left ? 0 : modifier.rightFlags;
    if (injected) {
        event.flags |= LLKHF_INJECTED | (left ? LLKHF_LOWER_IL_INJECTED : 0);
    }
    return event;
}

void send(HotkeyService& service, KBDLLHOOKSTRUCT event, bool down, bool systemKey) {
    if (!down) {
        event.flags |= LLKHF_UP;
    }
    const WPARAM message = systemKey
        ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP)
        : (down ? WM_KEYDOWN : WM_KEYUP);
    HotkeyServiceTestAccess::event(service, message, event);
}

void testOverlappingSides() {
    for (const auto& modifier : kModifiers) {
        for (unsigned downForms = 0; downForms < 4; ++downForms) {
            for (unsigned upForms = 0; upForms < 4; ++upForms) {
                for (unsigned order = 0; order < 4; ++order) {
                    for (unsigned injectedSides = 0; injectedSides < 4; ++injectedSides) {
                        HotkeyService service;
                        const bool firstDownLeft = (order & 1) == 0;
                        const bool firstUpLeft = (order & 2) == 0;
                        const bool systemKey = modifier.mask == MOD_ALT || (order & 1) != 0;
                        const auto event = [&](bool left, unsigned forms) {
                            const unsigned bit = left ? 1 : 2;
                            return modifierEvent(modifier, left, (forms & bit) != 0, (injectedSides & bit) != 0);
                        };
                        const std::string context = std::string(modifier.name) +
                            " down=" + std::to_string(downForms) + " up=" + std::to_string(upForms) +
                            " order=" + std::to_string(order) + " injected=" + std::to_string(injectedSides);
                        const auto expectHeld = [&] {
                            expect(HotkeyServiceTestAccess::modifiers(service) == modifier.mask,
                                context + ": held modifier lost");
                            expect(HotkeyServiceTestAccess::matches(service, VK_F9, modifier.mask, VK_F9),
                                context + ": normal key did not match");
                        };

                        send(service, event(firstDownLeft, downForms), true, systemKey);
                        send(service, event(firstDownLeft, downForms), true, systemKey);
                        expectHeld();
                        send(service, event(!firstDownLeft, downForms), true, systemKey);
                        expectHeld();
                        send(service, event(firstUpLeft, upForms), false, systemKey);
                        expectHeld();
                        send(service, event(!firstUpLeft, upForms), false, systemKey);
                        expect(HotkeyServiceTestAccess::modifiers(service) == 0,
                            context + ": modifier remained after both releases");
                        expect(!HotkeyServiceTestAccess::matches(service, VK_F9, modifier.mask, VK_F9),
                            context + ": released modifier still matched");
                        expect(HotkeyServiceTestAccess::matches(service, VK_F9, 0, VK_F9),
                            context + ": bare key did not match");
                    }
                }
            }
        }
    }
}

void testNormalMatchingAndInjectedKeyUp() {
    HotkeyService service;
    send(service, modifierEvent(kModifiers[0], true, false, false), true, false);
    send(service, modifierEvent(kModifiers[2], false, true, true), true, true);
    constexpr uint32_t held = MOD_CONTROL | MOD_SHIFT;
    expect(HotkeyServiceTestAccess::modifiers(service) == held, "different modifiers did not combine");
    expect(HotkeyServiceTestAccess::matches(service, VK_F9, held, VK_F9), "configured key did not match");
    expect(HotkeyServiceTestAccess::matches(service, VK_F9, held | MOD_WIN | MOD_NOREPEAT, VK_F9),
        "unsupported modifier bits were not masked");
    expect(!HotkeyServiceTestAccess::matches(service, VK_F10, held, VK_F9), "wrong key matched");
    expect(!HotkeyServiceTestAccess::matches(service, VK_F9, MOD_CONTROL, VK_F9), "extra modifier matched");
    expect(!HotkeyServiceTestAccess::matches(service, VK_F9, held | MOD_ALT, VK_F9), "missing modifier matched");
    expect(!HotkeyServiceTestAccess::matches(service, 0, held, 0), "unbound key matched");

    HotkeyServiceTestAccess::markPressed(service);
    KBDLLHOOKSTRUCT key{};
    key.vkCode = VK_F9;
    key.flags = LLKHF_INJECTED;
    send(service, key, false, true);
    expect(!HotkeyServiceTestAccess::startStopPressed(service), "injected key-up did not release start/stop");
    expect(HotkeyServiceTestAccess::saveReplayPressed(service), "wrong key-up released save replay");
    key.vkCode = VK_F10;
    send(service, key, false, false);
    expect(!HotkeyServiceTestAccess::saveReplayPressed(service), "injected key-up did not release save replay");
    expect(HotkeyServiceTestAccess::modifiers(service) == held, "normal keys changed modifiers");

    for (const auto& modifier : kModifiers) {
        send(service, modifierEvent(modifier, true, false, false), true, false);
        send(service, modifierEvent(modifier, false, false, false), true, false);
    }
    service.unregisterHotkeys(nullptr);
    expect(HotkeyServiceTestAccess::modifiers(service) == 0, "unregister did not clear both sides");
}

void testGenericShiftWithoutScanCode() {
    HotkeyService service;
    auto unknownShift = modifierEvent(kModifiers[2], true, true, true);
    unknownShift.scanCode = 0;
    send(service, unknownShift, true, false);
    send(service, modifierEvent(kModifiers[2], false, false, false), true, false);
    send(service, unknownShift, false, false);
    expect(HotkeyServiceTestAccess::modifiers(service) == MOD_SHIFT,
        "generic shift without a scan code cleared right shift");
    send(service, modifierEvent(kModifiers[2], false, false, false), false, false);
    expect(HotkeyServiceTestAccess::modifiers(service) == 0, "shift remained after releases");
}

} // namespace

int main() {
    try {
        testOverlappingSides();
        testNormalMatchingAndInjectedKeyUp();
        testGenericShiftWithoutScanCode();
    } catch (const std::exception& e) {
        std::cerr << "HotkeyServiceTests FAILED: " << e.what() << "\n";
        return 1;
    }
    std::cout << "HotkeyServiceTests passed\n";
    return 0;
}
