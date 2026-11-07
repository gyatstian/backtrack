#include "ui/MainWindow.h"
#include "ui/UiIds.h"

#include <Windows.h>
#include <objbase.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace backtrack {

struct MainWindowAccessibilityTestAccess {
    static HWND window(const MainWindow& window) { return window.window_; }
    static HWND host(const MainWindow& window, size_t index) { return window.pageCaches_[index].host; }
    static HWND tab(const MainWindow& window) { return window.tabButtons_.front(); }
    static void showPage(MainWindow& window, size_t index) {
        window.switchPage(static_cast<MainWindow::Page>(index));
    }
    static bool shortcut(MainWindow& window, MSG& message) { return window.handleShortcut(message); }
    static void stopWorker(MainWindow& window) { window.stopControllerWorker(); }
};

} // namespace backtrack

namespace {

using namespace backtrack;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct WindowCleanup {
    MainWindow& window;
    ~WindowCleanup() {
        const HWND handle = MainWindowAccessibilityTestAccess::window(window);
        if (handle && IsWindow(handle)) DestroyWindow(handle);
    }
};

struct KeyboardState {
    std::array<BYTE, 256> original{};
    KeyboardState() { expect(GetKeyboardState(original.data()) != FALSE, "GetKeyboardState failed"); }
    ~KeyboardState() { SetKeyboardState(original.data()); }
    void shift(bool pressed) const {
        auto state = original;
        state[VK_SHIFT] = pressed ? 0x80 : 0;
        state[VK_LSHIFT] = pressed ? 0x80 : 0;
        state[VK_RSHIFT] = 0;
        expect(SetKeyboardState(state.data()) != FALSE, "SetKeyboardState failed");
    }
};

HWND tab(MainWindow& window, const KeyboardState& keyboard, bool reverse) {
    keyboard.shift(reverse);
    MSG message{};
    message.hwnd = GetFocus();
    message.message = WM_KEYDOWN;
    message.wParam = VK_TAB;
    expect(message.hwnd != nullptr, "test window does not have keyboard focus");
    expect(!MainWindowAccessibilityTestAccess::shortcut(window, message), "shortcut swallowed Tab");
    expect(IsDialogMessageW(MainWindowAccessibilityTestAccess::window(window), &message) != FALSE,
           "dialog navigation did not handle Tab");
    return GetFocus();
}

void checkPage(MainWindow& window, size_t index, HWND hiddenHost, const KeyboardState& keyboard) {
    MainWindowAccessibilityTestAccess::showPage(window, index);
    const HWND root = MainWindowAccessibilityTestAccess::window(window);
    const HWND host = MainWindowAccessibilityTestAccess::host(window, index);
    expect(IsWindowVisible(host) != FALSE, "selected page is hidden");
    expect(IsWindowVisible(hiddenHost) == FALSE, "cached diagnostics page is not hidden");
    SetActiveWindow(root);
    SetFocus(MainWindowAccessibilityTestAccess::tab(window));
    const HWND start = GetFocus();
    expect(start == MainWindowAccessibilityTestAccess::tab(window), "could not focus a main-window tab");

    std::vector<HWND> order{start};
    bool wrapped = false;
    for (int count = 0; count < 256; ++count) {
        const HWND next = tab(window, keyboard, false);
        expect(next && IsWindowVisible(next) && IsWindowEnabled(next), "Tab selected a hidden or disabled control");
        expect(!IsChild(hiddenHost, next), "Tab entered the hidden cached page");
        if (next == start) {
            wrapped = true;
            break;
        }
        order.push_back(next);
    }
    expect(wrapped, "Tab did not wrap around the main window");
    bool entered = false;
    bool left = false;
    for (size_t i = 0; i < order.size(); ++i) {
        const bool before = IsChild(host, order[i]) != FALSE;
        const bool after = IsChild(host, order[(i + 1) % order.size()]) != FALSE;
        entered |= !before && after;
        left |= before && !after;
    }
    expect(entered && left, "Tab could not enter and leave the visible page");
    for (size_t remaining = order.size(); remaining > 0; --remaining) {
        const HWND previous = tab(window, keyboard, true);
        expect(previous == order[remaining - 1], "Shift+Tab did not reverse the traversal");
        expect(!IsChild(hiddenHost, previous), "Shift+Tab entered the hidden cached page");
    }
}

void testPageNavigation() {
    const HRESULT com = OleInitialize(nullptr);
    expect(SUCCEEDED(com), "OleInitialize failed");
    struct OleCleanup { ~OleCleanup() { OleUninitialize(); } } ole;

    const auto directory = std::filesystem::temp_directory_path() /
        (L"backtrack_ui_accessibility_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    struct DirectoryCleanup {
        std::filesystem::path path;
        ~DirectoryCleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{directory};

    AppSettings settings;
    settings.clipDirectory = directory;
    settings.checkForUpdates = false;
    settings.replay.enabled = false;
    settings.replay.diskBackedVideo = false;
    settings.replay.diskBackedAudio = false;
    settings.hotkeys.startStopVirtualKey = 0;
    settings.hotkeys.saveReplayVirtualKey = 0;
    RecorderController controller;
    expect(controller.initialize(settings), "controller initialization failed");
    SettingsStore store(directory / L"settings.ini");
    MainWindow window(controller, store, settings, SettingsStore::LoadStatus::Missing);
    WindowCleanup windowCleanup{window};
    expect(window.create(GetModuleHandleW(nullptr), SW_SHOWNORMAL, false), "main window creation failed");
    MainWindowAccessibilityTestAccess::stopWorker(window);
    MSG pending{};
    while (PeekMessageW(&pending, MainWindowAccessibilityTestAccess::window(window),
                        ui_ids::kControllerActionCompleteMessage, ui_ids::kControllerActionCompleteMessage, PM_REMOVE)) {
        DispatchMessageW(&pending);
    }

    for (size_t index = 0; index < 4; ++index) {
        const HWND host = MainWindowAccessibilityTestAccess::host(window, index);
        expect(host && (GetWindowLongPtrW(host, GWL_EXSTYLE) & WS_EX_CONTROLPARENT),
               "page host does not permit dialog traversal");
    }
    MainWindowAccessibilityTestAccess::showPage(window, 3);
    const HWND hiddenHost = MainWindowAccessibilityTestAccess::host(window, 3);
    expect(GetDlgItem(hiddenHost, ui_ids::kSaveLogButtonId) != nullptr, "diagnostics page was not built");
    const KeyboardState keyboard;
    checkPage(window, 0, hiddenHost, keyboard);
    checkPage(window, 2, hiddenHost, keyboard);
    checkPage(window, 1, hiddenHost, keyboard);
    checkPage(window, 3, MainWindowAccessibilityTestAccess::host(window, 0), keyboard);
}

} // namespace

int main() {
    try {
        testPageNavigation();
        std::cout << "MainWindowAccessibilityTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MainWindowAccessibilityTests failed: " << error.what() << '\n';
        return 1;
    }
}
