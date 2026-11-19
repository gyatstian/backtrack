#include "app/RecorderController.h"
#include "mux/RecoveryStore.h"
#include "platform/Win32Util.h"
#include "studio/GalleryLayout.h"
#include "studio/StudioMotion.h"
#include "studio/StudioTheme.h"
#include "studio/StudioWindow.h"

#include <CommCtrl.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace backtrack;
using namespace std::chrono_literals;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

HWND find(HWND parent, const wchar_t* text, bool visible = true) {
    struct Query { std::wstring text; bool visible; HWND hit; } query{text, visible, nullptr};
    EnumChildWindows(parent, [](HWND child, LPARAM arg) -> BOOL {
        auto& query = *reinterpret_cast<Query*>(arg);
        wchar_t value[256]{};
        GetWindowTextW(child, value, static_cast<int>(std::size(value)));
        if ((query.visible && !IsWindowVisible(child)) || value != query.text) return TRUE;
        query.hit = child;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&query));
    return query.hit;
}

void click(HWND parent, const wchar_t* text) {
    const auto button = find(parent, text);
    expect(button && IsWindowEnabled(button), "expected an enabled button");
    SendMessageW(button, BM_CLICK, 0, 0);
}

RECT bounds(HWND hwnd) {
    RECT rect{};
    expect(GetWindowRect(hwnd, &rect), "expected control bounds");
    return rect;
}

template <class Change>
void expectDeferredMove(HWND child, Change change, int expectedMoves = 1) {
    struct Observation { int moves = 0; bool deferred = true; } observed;
    SUBCLASSPROC watch = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) -> LRESULT {
        if (msg == WM_WINDOWPOSCHANGING) {
            const auto flags = reinterpret_cast<const WINDOWPOS*>(lp)->flags;
            if ((flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE)) {
                auto& seen = *reinterpret_cast<Observation*>(data);
                ++seen.moves;
                constexpr UINT deferPaint = SWP_NOREDRAW | SWP_NOCOPYBITS;
                seen.deferred &= (flags & deferPaint) == deferPaint;
            }
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    };
    expect(SetWindowSubclass(child, watch, 42, reinterpret_cast<DWORD_PTR>(&observed)), "watch layout moves");
    const bool changed = change();
    RemoveWindowSubclass(child, watch, 42);
    expect(changed && observed.moves == expectedMoves && observed.deferred,
           "layout should only move controls that need it, without drawing intermediate positions");
}

void expectScrollRepaint(HWND child) {
    const auto viewport = GetParent(child);
    const auto before = bounds(child);
    const auto initial = GetScrollPos(viewport, SB_VERT);
    RedrawWindow(viewport, nullptr, nullptr, RDW_VALIDATE | RDW_NOERASE | RDW_NOFRAME | RDW_ALLCHILDREN);
    expectDeferredMove(child, [&] { SendMessageW(viewport, WM_VSCROLL, SB_LINEDOWN, 0); return true; });
    const auto delta = GetScrollPos(viewport, SB_VERT) - initial;
    expect(delta > 0 && bounds(child).top == before.top - delta, "scroll should move the same child by the scrollbar delta");
    RECT client{};
    GetClientRect(child, &client);
    const auto region = CreateRectRgn(0, 0, 0, 0);
    const bool repaint = GetUpdateRgn(child, region, FALSE) != ERROR &&
        PtInRegion(region, client.right / 2, client.bottom / 2);
    DeleteObject(region);
    expect(repaint && GetUpdateRect(viewport, nullptr, FALSE), "scroll should repaint the moved child and viewport together");
}

template <class Test>
bool waitFor(Test test) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (test()) return true;
        Sleep(10);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void testStudioMotion() {
    const auto start = SteadyClock::time_point{};
    StudioMotion motion{0.f, 3.f, start, 200ms};
    const auto reverse = start + 100ms;
    const auto current = motion.value(reverse, true);
    expect(motion.value(start, true) == 0.f && current > 1.f && current < 2.f,
           "a long navigation hop should cover intermediate tabs rather than crossfade the ends");
    motion.retarget(0.f, reverse, 200ms, true);
    expect(motion.value(reverse, true) == current && motion.value(reverse + 16ms, true) < current,
           "reversing a transition should continue from the current value");
    expect(!motion.finished(reverse + 199ms) && motion.finished(reverse + 200ms) && motion.value(reverse + 1s, true) == 0.f,
           "a late frame should settle immediately at the final tab");
    const StudioMotion confirmation{0.f, 1.f, start, 160ms};
    expect(confirmation.value(start + 40ms, false) > 0.6f && confirmation.value(start + 160ms, false) == 1.f,
           "confirmation should respond quickly and finish at the final shape");
}

COLORREF frameBelowTab(HWND window, HWND tab, bool leftEdge = false) {
    const auto r = bounds(tab);
    POINT sample{leftEdge ? r.left : (r.left + r.right) / 2, r.bottom};
    ScreenToClient(window, &sample);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), 1, 1, 1, 32, BI_RGB};
    void* pixels = nullptr;
    const auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const auto dc = CreateCompatibleDC(nullptr);
    const auto old = bitmap && dc ? SelectObject(dc, bitmap) : nullptr;
    COLORREF color = CLR_INVALID;
    if (old) {
        SetViewportOrgEx(dc, -sample.x, -sample.y, nullptr);
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        color = GetPixel(dc, sample.x, sample.y);
        SelectObject(dc, old);
    }
    if (dc) DeleteDC(dc);
    if (bitmap) DeleteObject(bitmap);
    return color;
}

void checkNavigationMotion(HWND window) {
    const auto capture = find(window, L"Capture");
    const auto settings = find(window, L"Settings");
    expect(frameBelowTab(window, capture) == studio_theme::muted &&
           frameBelowTab(window, capture, true) == studio_theme::muted,
           "Capture should retain the neutral underline across the tab width");
    BOOL motionEnabled = FALSE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &motionEnabled, 0);
    auto tab = bounds(settings);
    POINT trackTop{tab.left, tab.bottom};
    ScreenToClient(window, &trackTop);
    struct TrackPaint { int top; int bottom; bool repainted = false; } track{
        trackTop.y, trackTop.y + MulDiv(studio_theme::navUnderlineBand, GetDpiForWindow(window), 96)};
    const SUBCLASSPROC observe = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) -> LRESULT {
        if (msg == WM_PAINT) {
            auto& track = *reinterpret_cast<TrackPaint*>(data);
            RECT dirty{};
            if (GetUpdateRect(hwnd, &dirty, FALSE) && dirty.top >= track.top && dirty.bottom <= track.bottom)
                track.repainted = true;
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    };
    expect(SetWindowSubclass(window, observe, 42, reinterpret_cast<DWORD_PTR>(&track)), "watch navigation strip repaints");
    const auto clicked = SteadyClock::now();
    constexpr LPARAM inside = MAKELPARAM(5, 5);
    SendMessageW(settings, WM_LBUTTONDOWN, MK_LBUTTON, inside);
    SendMessageW(settings, WM_LBUTTONUP, 0, inside);
    const bool pageReady = find(window, L"Check for updates at startup") != nullptr;
    const auto arrival = frameBelowTab(window, settings);
    const bool early = SteadyClock::now() - clicked < 50ms;
    const bool arrived = waitFor([&] {
        return (!motionEnabled || track.repainted) && frameBelowTab(window, settings) == studio_theme::muted;
    });
    RemoveWindowSubclass(window, observe, 42);
    expect(pageReady, "pointer navigation must open the page without waiting for the underline");
    if (!motionEnabled) expect(arrival == studio_theme::muted, "reduced motion should snap to the new tab");
    else if (early) expect(arrival == studio_theme::surface, "pointer navigation should travel from the previous tab");
    expect(arrived, "the underline timer should repaint just its band and reach Settings");
    expect(frameBelowTab(window, find(window, L"App")) == studio_theme::muted, "Settings should paint an independent category underline");
    click(window, L"Video");
    expect(frameBelowTab(window, find(window, L"Video")) == studio_theme::muted, "keyboard-style activation should snap the category underline");
    click(window, L"App");
    SendMessageW(capture, BM_CLICK, 0, 0);
    expect(frameBelowTab(window, capture) == studio_theme::muted, "keyboard-style activation should snap the main underline");
}

void testGalleryGeometry() {
    const StudioGalleryLayout layout{3, 100};
    expect(layout.rowCount(0) == 0 && layout.rowCount(7) == 3, "partial last row should exist");
    expect(layout.visible(20, 100, 100) == std::pair<size_t, size_t>{3, 6}, "exact viewport should not include another row");
    expect(layout.visible(7, 195, 10, 1) == std::pair<size_t, size_t>{3, 7}, "overscan must stop at the last clip");
    expect(layout.visible(20, 100, 0, 1) == std::pair<size_t, size_t>{0, 0}, "zero-height viewport should have no tiles or overscan");
}

template <class Test>
void withHeldStartup(AppSettings initial, SettingsStore& store, Test test) {
    initial.replay.enabled = false;
    initial.replay.diskBackedVideo = initial.replay.diskBackedAudio = false;
    initial.checkForUpdates = false;
    initial.hotkeys = {0, 0, 0, 0};
    initial = sanitizeSettings(initial);
    RecorderController controller;
    const auto uiThread = GetCurrentThreadId();
    std::atomic<DWORD> prepareThread{0};
    std::atomic<bool> prepareTimedOut{false};
    std::promise<void> release;
    StudioWindow studio(controller, store, initial, SettingsStore::LoadStatus::Loaded,
        [&prepareThread, &prepareTimedOut, gate = release.get_future().share()] {
            prepareThread = GetCurrentThreadId();
            prepareTimedOut = gate.wait_for(8s) != std::future_status::ready;
        });
    // Abandoning the promise on failure releases the wait before Studio joins its worker.
    auto releaseOnExit = std::move(release);
    expect(studio.create(GetModuleHandleW(nullptr), SW_SHOWNOACTIVATE, false), "studio window should create");
    HWND window = nullptr;
    EnumThreadWindows(uiThread, [](HWND hwnd, LPARAM result) -> BOOL {
        wchar_t name[64]{};
        GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
        if (std::wstring_view(name) != kBacktrackMainWindowClassName) return TRUE;
        *reinterpret_cast<HWND*>(result) = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&window));
    expect(window != nullptr, "studio window should be discoverable for activation");
    expect(waitFor([&] { return prepareThread.load() != 0; }), "capture preparation should start");
    expect(prepareThread.load() != uiThread, "capture preparation must not run on the create thread");
    test(window, controller, [&] { releaseOnExit.set_value(); });
    expect(!prepareTimedOut.load(), "the test must open the preparation gate before its watchdog expires");
}

void checkTitleBar(HWND window) {
    const auto clientBounds = [&] {
        RECT rect{};
        GetClientRect(window, &rect);
        MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&rect), 2);
        return rect;
    };
    const auto titlePoint = [&] {
        const auto client = clientBounds();
        return MAKELPARAM((client.left + client.right) / 2, (client.top + bounds(find(window, L"Capture")).top) / 2);
    };
    const auto hit = [&](LPARAM point) { return SendMessageW(window, WM_NCHITTEST, 0, point); };
    const auto original = bounds(window);
    const auto client = clientBounds();
    expect(EqualRect(&original, &client), "the custom title bar should replace the native frame on first show");
    expect(hit(MAKELPARAM(client.left, client.top)) == HTTOPLEFT, "restored window should have resize grips");
    const auto closeButton = find(window, L"Close");
    const auto close = bounds(closeButton);
    expect(hit(MAKELPARAM((close.left + close.right) / 2, (close.top + close.bottom) / 2)) == HTCLIENT,
           "the close button should not start a window drag");
    const auto checkGrip = [&](POINT screen, LRESULT expected) {
        POINT point = screen;
        ScreenToClient(window, &point);
        expect(ChildWindowFromPointEx(window, point, CWP_SKIPINVISIBLE) == window,
               "resize grips must not be covered by child windows");
        expect(hit(MAKELPARAM(screen.x, screen.y)) == expected, "window edge should hit-test as a resize grip");
    };
    const auto navY = bounds(find(window, L"Capture")).top;
    const auto midY = (client.top + client.bottom) / 2;
    checkGrip({(close.left + close.right) / 2, client.top}, HTTOP);
    checkGrip({client.right - 1, client.top}, HTTOPRIGHT);
    checkGrip({client.left, navY}, HTLEFT);
    checkGrip({client.right - 1, navY}, HTRIGHT);
    checkGrip({client.left, midY}, HTLEFT);
    checkGrip({client.right - 1, midY}, HTRIGHT);
    checkGrip({(client.left + client.right) / 2, client.bottom - MulDiv(4, GetDpiForWindow(window), 96)}, HTBOTTOM);
    checkGrip({client.right - 1, client.bottom - 1}, HTBOTTOMRIGHT);

    const LONG w = close.right - close.left, h = close.bottom - close.top;
    BITMAPINFO bitmap{{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
    void* pixels = nullptr;
    const auto dc = CreateCompatibleDC(nullptr);
    const auto surface = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const auto previous = dc && surface ? SelectObject(dc, surface) : nullptr;
    bool smooth = false, centered = false, untouched = false;
    if (previous) {
        centered = untouched = true;
        for (const auto button : {closeButton, find(window, L"Minimize")}) {
            constexpr auto marker = RGB(1, 2, 3);
            SetPixelV(dc, w / 2, h / 2, marker);
            untouched &= SendMessageW(button, WM_ERASEBKGND, reinterpret_cast<WPARAM>(dc), 0) != 0 &&
                         GetPixel(dc, w / 2, h / 2) == marker;
            DRAWITEMSTRUCT item{ODT_BUTTON, 0, 0, ODA_DRAWENTIRE, ODS_SELECTED, button, dc, {0, 0, w, h}};
            SendMessageW(window, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&item));
            const auto background = GetPixel(dc, 0, 0), ink = GetPixel(dc, w / 2, h / 2);
            centered &= ink != CLR_INVALID && ink != background;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const auto pixel = GetPixel(dc, x, y);
                    centered &= pixel == GetPixel(dc, w - 1 - x, y) && pixel == GetPixel(dc, x, h - 1 - y);
                    if (button == closeButton && pixel != CLR_INVALID && pixel != ink && pixel != background) smooth = true;
                }
            }
        }
        SelectObject(dc, previous);
    }
    if (surface) DeleteObject(surface);
    if (dc) DeleteDC(dc);
    expect(smooth, "the close icon should have blended edge pixels");
    expect(centered, "both caption icons should be centered in their button bounds");
    expect(untouched, "owner-drawn buttons should not flash a separate background erase");
    auto point = titlePoint();
    expect(hit(point) == HTCAPTION, "the title strip should support native dragging");
    SendMessageW(window, WM_NCLBUTTONDBLCLK, HTCAPTION, point);
    expect(IsZoomed(window), "double-clicking the title strip should maximize");
    MONITORINFO monitor{sizeof(monitor)};
    expect(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor), "expected monitor work area");
    const auto maximized = clientBounds();
    expect(EqualRect(&maximized, &monitor.rcWork), "maximized client area should fit the work area");
    expect(hit(MAKELPARAM((maximized.left + maximized.right) / 2, maximized.top)) == HTCAPTION,
           "the maximized title strip should not have a resize grip");
    point = titlePoint();
    SendMessageW(window, WM_NCLBUTTONDBLCLK, HTCAPTION, point);
    const auto restored = bounds(window);
    expect(!IsZoomed(window) && EqualRect(&original, &restored), "double-clicking again should restore the original bounds");
}

void testCloseDuringStartup(const std::filesystem::path& temporary) {
    const auto clips = temporary / L"close-clips";
    std::filesystem::create_directories(clips);
    SettingsStore store(temporary / L"close-settings.ini");
    withHeldStartup(AppSettings{.clipDirectory = clips}, store, [&](HWND window, RecorderController&, auto release) {
        checkTitleBar(window);
        const auto tab = find(window, L"Capture");
        const auto previous = bounds(tab);
        const auto frame = bounds(window);
        expectDeferredMove(tab, [&] {
            return SetWindowPos(window, nullptr, frame.left + 24, frame.top, frame.right - frame.left - 24,
                                frame.bottom - frame.top, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
        });
        expect(bounds(tab).right != previous.right && GetUpdateRect(tab, nullptr, FALSE),
               "edge resize should lay out and invalidate resized buttons");
        click(window, L"Diagnostics");
        const auto logging = find(window, L"Enable logging");
        expect(logging && SendMessageW(logging, BM_GETCHECK, 0, 0) == BST_CHECKED, "logging should start enabled");
        click(window, L"Enable logging");
        expect(SendMessageW(logging, BM_GETCHECK, 0, 0) == BST_CHECKED && !std::filesystem::exists(store.path()),
               "logging should restore the checkbox without writing settings during startup");

        SendMessageW(window, WM_CLOSE, 0, 0);
        expect(IsWindow(window), "WM_CLOSE should return with the window alive while preparation is held");
        release();
        expect(waitFor([&] { return !IsWindow(window); }), "Studio should close after preparation is released");
    });
}

void checkStudioPages(HWND window, const SettingsStore& store, const std::filesystem::path& example,
                      const AppSettings& initial) {
    const auto clips = example.parent_path();
    const auto record = find(window, L"Start recording");
    const auto recordHotkey = GetWindow(record, GW_HWNDNEXT);
    expect(!find(window, L"Recover failed recording"), "no recovery action without a recoverable session");
    const auto enableReplay = find(window, L"Enable replay");
    const auto replayHotkey = enableReplay ? GetWindow(enableReplay, GW_HWNDNEXT) : nullptr;
    const auto saveSettings = find(window, L"Save settings");
    const auto recordRect = bounds(record), hotkeyRect = bounds(recordHotkey);
    const auto enableRect = bounds(enableReplay), replayHotkeyRect = bounds(replayHotkey);
    expect(recordRect.top == hotkeyRect.top && recordRect.right < hotkeyRect.left,
           "recording action should sit next to its shortcut");
    expect(!IsWindowVisible(replayHotkey), "the replay shortcut should stay hidden while replay is off");
    expect(!find(window, L"Save") && !find(window, L"the last"),
           "replay length should stay hidden while replay is off");
    click(window, L"Enable replay");
    const auto replayButton = find(window, L"Save");
    const auto lastLabel = find(window, L"the last");
    const auto input = lastLabel ? GetWindow(lastLabel, GW_HWNDNEXT) : nullptr;
    const auto secondsLabel = input ? GetWindow(input, GW_HWNDNEXT) : nullptr;
    expect(replayButton && lastLabel && input && secondsLabel,
           "enabling replay should reveal Save, the last, the length input and seconds");
    const auto replayRect = bounds(replayButton), lastRect = bounds(lastLabel);
    const auto inputRect = bounds(input), secondsRect = bounds(secondsLabel);
    expect(IsWindowVisible(replayHotkey) && replayHotkeyRect.top == enableRect.top &&
           enableRect.right < replayHotkeyRect.left,
           "the replay shortcut should appear next to Enable replay");
    expect(replayRect.top == lastRect.top && replayRect.top > enableRect.top && replayRect.right < lastRect.left &&
           lastRect.right < inputRect.left && inputRect.right < secondsRect.left,
           "replay should read Save the last [x] seconds on the row below its enable checkbox");
    const auto before = bounds(input), saveSettingsRect = bounds(saveSettings);
    RECT after{};
    SendMessageW(input, WM_MOUSEMOVE, 0, MAKELPARAM(3, 3));
    const auto captureHelp = find(window, L"Longer replay buffers retain more footage and take more disk or memory space.");
    RECT captureHelpRect{};
    expect(GetWindowRect(captureHelp, &captureHelpRect) && captureHelpRect.left > saveSettingsRect.right,
           "hover should reveal help beside Save settings");
    GetWindowRect(input, &after);
    expect(EqualRect(&before, &after), "footer must not move the hovered field");
    SendMessageW(input, WM_MOUSELEAVE, 0, 0);
    expect(!find(window, L"Longer replay buffers retain more footage and take more disk or memory space."), "footer should hide after hover");
    click(window, L"Enable replay");
    expect(!IsWindowVisible(input) && !IsWindowVisible(replayButton) && !IsWindowVisible(replayHotkey) &&
           GetParent(saveSettings) == window && !IsWindowEnabled(saveSettings),
           "turning replay off should hide its row and shortcut and restore a clean draft");

    click(window, L"Diagnostics");
    const auto logging = find(window, L"Enable logging");
    const auto runtime = FindWindowExW(GetParent(logging), nullptr, L"EDIT", nullptr);
    const auto encoder = FindWindowExW(GetParent(logging), runtime, L"EDIT", nullptr);
    const auto log = FindWindowExW(GetParent(logging), encoder, L"EDIT", nullptr);
    const auto openLog = bounds(find(window, L"Open log folder"));
    const auto saveLog = bounds(find(window, L"Save log\u2026"));
    expect(bounds(runtime).right < bounds(encoder).left && bounds(encoder).right < bounds(logging).left &&
           bounds(logging).bottom < openLog.top && openLog.top == saveLog.top && openLog.right < saveLog.left &&
           openLog.bottom < bounds(log).top && bounds(log).left == bounds(logging).left,
           "recent log controls should be grouped in the right-hand diagnostics column above the log");

    click(window, L"Library");
    expect(waitFor([&] { return find(window, L"Example.mp4") != nullptr; }), "scan should publish a named gallery tile");
    expect(!find(window, L"List"), "studio library should not offer a list view");
    const auto open = bounds(find(window, L"Open"));
    const auto rename = bounds(find(window, L"Rename"));
    const auto favorite = bounds(find(window, L"Favorite"));
    const auto refresh = bounds(find(window, L"Refresh"));
    const auto remove = bounds(find(window, L"Delete"));
    expect(open.top == rename.top && rename.top == favorite.top && favorite.top == refresh.top && refresh.top == remove.top &&
           open.left < rename.left && rename.left < favorite.left && favorite.right < refresh.left &&
           refresh.right < remove.left && remove.bottom < bounds(find(window, L"Clip gallery")).top,
           "library actions should read Open, Rename, Favorite from the left and Refresh, Delete against the right edge above the gallery");
    const auto tile = find(window, L"Example.mp4");
    expectScrollRepaint(tile);
    expect(find(window, L"Example.mp4") == tile, "small scroll should retain the tile and its clip");
    click(window, L"Example.mp4");
    click(window, L"Favorite");
    expect(waitFor([&] { return find(window, L"Unfavorite") != nullptr; }), "favorite edit should update the selected tile action");

    // Only a new scan can move this clip ahead of the example.
    std::filesystem::last_write_time(clips / L"0.mp4", std::filesystem::file_time_type::clock::now() + 2h);
    const auto renameButton = find(window, L"Rename");
    const auto renameEdit = FindWindowExW(find(window, L"Clip gallery"), nullptr, L"Edit", nullptr);
    expect(renameEdit && !IsWindowVisible(renameEdit), "the rename input should stay hidden until Rename is clicked");
    click(window, L"Rename");
    RECT tileRect{}, editRect{};
    expect(IsWindowVisible(renameEdit) && GetFocus() == renameEdit, "Rename should turn the selected clip name into an input");
    expect(GetWindowRect(find(window, L"Example.mp4"), &tileRect) && GetWindowRect(renameEdit, &editRect) &&
           editRect.left >= tileRect.left && editRect.right <= tileRect.right &&
           editRect.top > tileRect.top && editRect.bottom < tileRect.bottom,
           "the rename input should replace the name line inside the selected tile");
    expect(SetWindowTextW(renameEdit, L"Renamed example"), "rename field should accept the new stem");
    SendMessageW(renameEdit, WM_KEYDOWN, VK_RETURN, 0);
    expect(!IsWindowEnabled(renameButton) && !IsWindowVisible(renameEdit),
           "Enter should commit the rename and clear the inline input");
    click(window, L"Refresh");
    expect(waitFor([&] {
        const auto renamed = find(window, L"Renamed example.mp4");
        const auto promoted = find(window, L"0.mp4");
        const auto unfavorite = find(window, L"Unfavorite");
        if (!renamed || !promoted || !unfavorite || !IsWindowEnabled(unfavorite) || find(window, L"Example.mp4")) return false;
        const auto first = bounds(promoted), second = bounds(renamed);
        return first.top < second.top || (first.top == second.top && first.left < second.left);
    }), "refresh should show the renamed tile and preserve its selected Unfavorite action");
    expect(std::filesystem::exists(clips / L"Renamed example.mp4") && !std::filesystem::exists(example),
           "renaming during refresh should move the clip on disk");

    const auto libraryTab = find(window, L"Library");
    const auto libraryTabRect = bounds(libraryTab);
    const auto dpi = GetDpiForWindow(window);
    const auto gap = MulDiv(8, dpi, 96);
    LOGFONTW font{};
    const auto tabFont = reinterpret_cast<HFONT>(SendMessageW(libraryTab, WM_GETFONT, 0, 0));
    expect(GetObjectW(tabFont, sizeof(font), &font) && font.lfHeight == -MulDiv(15, dpi, 96),
           "studio text should use the larger DPI-scaled font");
    expect(bounds(find(window, L"Settings")).left - libraryTabRect.right == gap,
           "main navigation buttons should have space between them");
    expect(!find(window, L"App"), "categories should be hidden outside Settings");
    click(window, L"Settings");
    const auto app = find(window, L"App");
    const auto appRect = bounds(app);
    const auto currentLibraryRect = bounds(libraryTab);
    expect(find(window, L"Library") == libraryTab && EqualRect(&libraryTabRect, &currentLibraryRect),
           "Settings should leave the main tabs visible and in place");
    expect(appRect.top - libraryTabRect.top == MulDiv(24, dpi, 96) &&
           appRect.top - libraryTabRect.bottom == MulDiv(3, dpi, 96),
           "the underline should sit within the original main navigation row height");
    expect(GetNextDlgTabItem(window, find(window, L"Diagnostics"), FALSE) == app,
           "Tab should move from the last main tab to the first settings category");

    expect(!find(window, L"Replay success sound"), "notification sound should not be on App");
    click(window, L"Audio");
    const auto output = GetWindow(find(window, L"Output device"), GW_HWNDNEXT);
    expect(output && SendMessageW(output, CB_FINDSTRINGEXACT, -1, reinterpret_cast<LPARAM>(L"Default")) != CB_ERR,
           "Audio should offer Default immediately without waiting for device discovery");
    const auto pinned = SendMessageW(output, CB_FINDSTRINGEXACT, -1,
                                     reinterpret_cast<LPARAM>(initial.audioOutputDeviceId.c_str()));
    expect(pinned != CB_ERR && SendMessageW(output, CB_GETCURSEL, 0, 0) == pinned,
           "Audio should immediately select the pinned unavailable output");
    expect(bounds(find(window, L"Replay success sound")).bottom < bounds(find(window, L"Capture system audio")).top,
           "notification sound should be above audio devices");
    expect(GetParent(find(window, L"Save settings")) == window, "every settings category should pin Save to the footer");
    const auto notificationVolume = GetWindow(find(window, L"Volume (%)"), GW_HWNDNEXT);
    expect(SetWindowTextW(notificationVolume, L"101"), "notification volume should accept a draft edit");
    click(window, L"Save settings");
    const auto normalizedSave = find(window, L"Save settings");
    expect(normalizedSave && !IsWindowEnabled(normalizedSave) && !std::filesystem::exists(store.path()),
           "an edit that normalizes to the saved volume should clear the draft without publishing settings");

    const auto captureCategory = GetNextDlgTabItem(window, app, FALSE);
    const auto captureCategoryRect = bounds(captureCategory);
    expect(captureCategoryRect.top == appRect.top && captureCategoryRect.left - appRect.right == gap,
           "settings category buttons should have the same spacing as the main navigation");
    SendMessageW(captureCategory, BM_CLICK, 0, 0);
    const auto overlapLabel = find(window, L"Clip overlap");
    const auto folderLabel = find(window, L"Buffer folder", false);
    const auto overlap = overlapLabel ? GetWindow(overlapLabel, GW_HWNDNEXT) : nullptr;
    const auto folder = folderLabel ? GetWindow(folderLabel, GW_HWNDNEXT) : nullptr;
    const auto save = find(window, L"Save");
    expect(overlap && folder && save, "capture preview should show clip overlap, buffer folder and Save");
    const auto body = GetParent(folder);
    expect(GetParent(save) == window && !IsWindowVisible(folder) && !IsWindowEnabled(save),
           "Save should be pinned and the buffer folder hidden until a disk track is chosen");
    click(body, L"Audio");
    expect(IsWindowVisible(folder) && IsWindowEnabled(save), "enabling disk audio should reveal the buffer folder and Save");
    const auto clipLabel = find(window, L"Clip folder");
    const auto clipFolder = clipLabel ? GetWindow(clipLabel, GW_HWNDNEXT) : nullptr;
    RECT overlapRect{}, clipRect{}, folderRect{}, saveRect{}, footerRect{};
    expect(GetWindowRect(overlap, &overlapRect) && GetWindowRect(clipFolder, &clipRect) &&
           GetWindowRect(folder, &folderRect) && GetWindowRect(save, &saveRect), "capture controls should have bounds");
    expect(bounds(folderLabel).left > bounds(clipLabel).left, "dependent rows should leave room for the guide on their left");
    expect(overlapRect.left == clipRect.left && std::abs(folderRect.left - clipRect.left) <= 1 && overlapRect.right < clipRect.right &&
           clipRect.bottom < folderRect.top && folderRect.bottom < overlapRect.top,
           "capture storage should precede overlap, with the buffer folder between them");
    click(body, L"Audio");
    expect(!IsWindowVisible(folder) && !IsWindowEnabled(save),
           "undoing the disk edit should restore the clean state and hide the buffer folder");
    const auto follow = GetWindow(find(window, L"Follow", false), GW_HWNDNEXT);
    const auto stabilize = find(window, L"Stabilize frames across monitors", false);
    expect(follow && stabilize && !IsWindowVisible(follow) && !IsWindowVisible(stabilize),
           "dependent monitor controls should be hidden while multi-monitor is off");
    click(body, L"Follow across monitors");
    RECT followRect{}, stabilizeRect{};
    expect(IsWindowVisible(follow) && IsWindowVisible(stabilize) && GetWindowRect(follow, &followRect) &&
           GetWindowRect(stabilize, &stabilizeRect) && followRect.bottom < stabilizeRect.top,
           "multi-monitor should reveal both dependent controls below its checkbox");
    click(body, L"Follow across monitors");
    expect(!IsWindowVisible(follow) && !IsWindowVisible(stabilize), "turning multi-monitor off should hide both dependent controls");
    expect(GetWindowRect(overlap, &overlapRect), "capture controls should have bounds after the layout settles");
    SendMessageW(overlap, WM_MOUSEMOVE, 0, MAKELPARAM(3, 3));
    const auto help = FindWindowExW(window, nullptr, L"Edit", nullptr);
    wchar_t footerText[512]{};
    GetWindowTextW(help, footerText, static_cast<int>(std::size(footerText)));
    expect(IsWindowVisible(help) && std::wstring_view(footerText).find(L"When saving again within the replay window") != std::wstring_view::npos,
           "overlap help should be shown even with other status text");
    GetWindowRect(help, &footerRect);
    GetWindowRect(overlap, &after);
    expect(EqualRect(&overlapRect, &after) && footerRect.top >= overlapRect.bottom && footerRect.left > saveRect.right,
           "help must stay below the fields and beside Save without moving the input");
    SendMessageW(overlap, WM_MOUSELEAVE, 0, 0);

    click(window, L"Video");
    expect(bounds(find(window, L"Resolution")).top < bounds(find(window, L"Adaptive mode")).top &&
           bounds(find(window, L"Adaptive mode")).top < bounds(find(window, L"Codec")).top &&
           bounds(find(window, L"Codec")).top < bounds(find(window, L"Profile")).top &&
           !find(window, L"Width") && !find(window, L"Preset") && !find(window, L"Stabilize frames across monitors"),
           "Video should order quality, GPU and format, hiding preset-only and multi-monitor rows");
    const auto resolution = find(window, L"Resolution");
    const auto videoViewport = GetParent(resolution);
    expect(videoViewport && GetScrollPos(videoViewport, SB_VERT) == 0, "Video should open at the top");
    expect(RedrawWindow(videoViewport, nullptr, nullptr, RDW_VALIDATE | RDW_NOERASE | RDW_NOFRAME | RDW_ALLCHILDREN),
           "Video viewport and children should validate before scrolling");
    SendMessageW(videoViewport, WM_VSCROLL, SB_LINEUP, 0);
    expect(GetScrollPos(videoViewport, SB_VERT) == 0 && !GetUpdateRect(videoViewport, nullptr, FALSE) &&
           !GetUpdateRect(resolution, nullptr, FALSE), "scrolling up at the top should not invalidate Video");
    expectScrollRepaint(resolution);

    const auto beforeDrag = bounds(resolution), frame = bounds(window);
    const auto resize = [&](int dx) {
        expect(SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left + dx, frame.bottom - frame.top,
                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "resize the Video page");
    };
    SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
    resize(-24);
    resize(-48);
    expect(bounds(resolution).right == beforeDrag.right, "resize input should not lay out each intermediate width");
    UpdateWindow(window);
    expect(bounds(resolution).right < beforeDrag.right, "Video should update during the resize drag once it paints");
    resize(0);
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
    expect(bounds(resolution).right == beforeDrag.right, "ending the drag should apply the final size before paint");
    for (const int dy : {-16, 0}) {
        expectDeferredMove(resolution, [&] {
            return SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top + dy,
                                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
        }, 0);
    }

    const auto beforeWheel = bounds(resolution);
    const auto scrollBeforeWheel = GetScrollPos(videoViewport, SB_VERT);
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines) {
        for (int i = 0; i < 8; ++i)
            SendMessageW(videoViewport, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
        expect(bounds(resolution).top == beforeWheel.top, "wheel bursts should not reposition controls before paint");
        UpdateWindow(videoViewport);
        expect(GetScrollPos(videoViewport, SB_VERT) == 0 && bounds(resolution).top == beforeWheel.top + scrollBeforeWheel,
               "painting should apply the accumulated wheel scroll to the original top position");
    }
    expect((GetWindowLongPtrW(videoViewport, GWL_EXSTYLE) & WS_EX_COMPOSITED) != 0,
           "the scroll viewport should paint its native children into one buffer");
    click(window, L"Integrations");
    expect(bounds(find(window, L"League of Legends kill reminder")).top < bounds(find(window, L"Voice command")).top &&
           bounds(find(window, L"Voice command")).top < bounds(find(window, L"Discord presence")).top &&
           bounds(find(window, L"Discord presence")).top < bounds(find(window, L"Record while a selected app is open")).top,
           "integration settings should order League, voice, Discord and auto record");
    click(window, L"Library");
    expect(find(window, L"Clip gallery") && !find(window, L"App"),
           "the main tabs should leave Settings and hide the category row");
    click(window, L"Capture");
    expect(find(window, L"Enable replay") != nullptr, "capture settings should be reachable after leaving settings");
    click(window, L"Enable replay");
    const auto bufferLabel = find(window, L"the last");
    const auto buffer = bufferLabel ? GetWindow(bufferLabel, GW_HWNDNEXT) : nullptr;
    expect(buffer != nullptr, "enabling replay should reveal its length field");
    SetWindowTextW(buffer, L"4294967296");
    click(window, L"Save settings");
    expect(!std::filesystem::exists(store.path()), "an out-of-range edit must not publish settings");
    const auto session = clips / L".backtrack_tmp" / L"failed";
    std::filesystem::create_directories(session);
    MuxedInputs failed;
    failed.videoPath = session / L"video.h264";
    failed.fps = 60;
    failed.width = 1920;
    failed.height = 1080;
    failed.videoSamples.push_back({.size = 4, .duration100ns = 166667, .keyFrame = true});
    std::ofstream(failed.videoPath, std::ios::binary) << "data";
    expect(RecoveryStore::writeManifest(failed, session), "create a session after automatic startup recovery");
    SetWindowTextW(buffer, L"45");
    click(window, L"Save settings");
    expect(store.load().replay.seconds == 45, "save should publish the edited draft");
    expect(waitFor([&] { return find(window, L"Recover failed recording") != nullptr; }),
           "recovery action should appear when the worker finds a candidate");
}

void checkAudioDraft(HWND window, const SettingsStore& store, const AppSettings& initial) {
    click(window, L"Settings");
    click(window, L"Audio");
    const auto gain = GetWindow(find(window, L"Desktop gain (%)"), GW_HWNDNEXT);
    const auto save = find(window, L"Save settings");
    expect(gain && IsWindowVisible(gain) && save && !IsWindowEnabled(save), "Audio should start with a visible gain and a clean draft");
    expect(SetWindowTextW(gain, L"8") && IsWindowEnabled(save), "a valid gain edit should dirty the draft");
    expect(SetWindowTextW(gain, L""), "gain field should accept clearing the edit");
    click(window, L"Capture system audio");
    expect(!IsWindowVisible(gain), "disabling system audio should hide the invalid gain field");
    click(window, L"App");
    expect(find(window, L"Check for updates at startup"), "disabled invalid gain should not block category navigation");
    click(window, L"Save settings");
    const auto saved = store.load();
    expect(!saved.captureSystemAudio, "save from App should persist disabled system audio");
    expect(saved.audioOutputVolumePercent == initial.audioOutputVolumePercent, "save must not persist the last valid gain prefix");
    expect(saved.audioOutputDeviceId == initial.audioOutputDeviceId, "save should retain the pinned unavailable output");
}

void testWindow(const std::filesystem::path& temporary) {
    const auto clips = temporary / L"clips";
    std::filesystem::create_directories(clips);
    const auto example = clips / L"Example.mp4";
    std::ofstream(example, std::ios::binary).put('\0');
    for (int i = 0; i < 20; ++i)
        std::ofstream(clips / (std::to_wstring(i) + L".mp4"), std::ios::binary).put('\0');
    std::filesystem::last_write_time(example, std::filesystem::file_time_type::clock::now() + 1h);
    SettingsStore store(temporary / L"settings.ini");
    const AppSettings initial{.clipDirectory = clips, .audioOutputDeviceId = L"studio-test-unavailable-output",
                              .audioOutputVolumePercent = 73};
    withHeldStartup(initial, store, [&](HWND window, RecorderController& controller, auto release) {
        const auto startupVisible = [&] {
            const auto footer = FindWindowExW(window, nullptr, L"Edit", nullptr);
            wchar_t text[256]{};
            GetWindowTextW(footer, text, static_cast<int>(std::size(text)));
            return IsWindowVisible(footer) && std::wstring_view(text).starts_with(L"Starting capture\u2026");
        };
        checkNavigationMotion(window);
        const auto captureTab = find(window, L"Capture");
        click(window, L"Settings");
        expect(find(window, L"App") && find(window, L"Check for updates at startup"),
               "Studio should open Settings while capture preparation is blocked");
        SendMessageW(captureTab, BM_CLICK, 0, 0);
        expect(find(window, L"Enable replay") && startupVisible(), "Studio should return to Capture before startup finishes");
        release();
        expect(waitFor([&] { return !startupVisible(); }), "startup action should finish after releasing preparation");
        expect(controller.settings().clipDirectory == clips, "startup should apply the initial settings");
        checkStudioPages(window, store, example, initial);
        checkAudioDraft(window, store, initial);
        DestroyWindow(window);
    });
}
} // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() / (L"BacktrackStudioTest-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    try {
        testStudioMotion();
        testGalleryGeometry();
        testCloseDuringStartup(path);
        testWindow(path);
        std::filesystem::remove_all(path, error);
        return 0;
    } catch (const std::exception& errorText) {
        std::cerr << errorText.what() << '\n';
        std::filesystem::remove_all(path, error);
        return 1;
    }
}
