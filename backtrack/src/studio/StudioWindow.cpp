#include "studio/StudioWindowImpl.h"
#include "studio/StudioTheme.h"

#include "core/AppVersion.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"
#include "resource.h"
#include "ui_common/WindowTools.h"

#include <CommCtrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string_view>

namespace backtrack {
namespace {
namespace theme = studio_theme;
constexpr wchar_t bodyClass[] = L"BacktrackStudioBody";
constexpr LPARAM hookHotkey = 0x4254484b;
constexpr std::array mainLabels{L"Capture", L"Library", L"Settings", L"Diagnostics"};
constexpr std::array settingsLabels{L"App", L"Capture", L"Video", L"Audio", L"Integrations"};

int extent(LONG a, LONG b) { return (std::max)(0L, b - a); }

bool registerWindowClass(const WNDCLASSW& wc) {
    return RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}
} // namespace

StudioWindow::StudioWindow(RecorderController& controller, SettingsStore& store, const AppSettings& settings,
                           SettingsStore::LoadStatus loadStatus, std::function<void()> prepareCapture)
    : impl_(std::make_unique<Impl>(controller, store, settings, loadStatus, std::move(prepareCapture))) {}
StudioWindow::~StudioWindow() = default;
bool StudioWindow::create(HINSTANCE instance, int showCommand, bool startMinimized) {
    return impl_->create(instance, showCommand, startMinimized);
}
int StudioWindow::runMessageLoop() { return impl_->run(); }

StudioWindow::Impl::Impl(RecorderController& controller, SettingsStore& store, const AppSettings& initial,
                         SettingsStore::LoadStatus loadStatus, Action prepareCapture)
    : controller(controller), store(store), prepareCapture(std::move(prepareCapture)), draft(initial), saved(initial),
      readable(loadStatus != SettingsStore::LoadStatus::Failed),
      league([this] { PostMessageW(window, leagueKill, 0, 0); }) {
    uiZoom = std::clamp(static_cast<int>(initial.uiZoomPercent), theme::minUiZoom, theme::maxUiZoom);
    libraryZoom = std::clamp(static_cast<int>(initial.libraryZoomPercent), theme::minUiZoom, theme::maxUiZoom);
    // A persisted zoom also sizes the first window, so it opens readable instead
    // of cramped; the window factor then measures the unzoomed default width.
    if (!initial.uiScaleWithWindow) {
        uiScale = studio_theme::uiScalePercent(uiZoom, 0, 0, false);
        galleryScale = initial.libraryOwnScale
            ? studio_theme::uiScalePercent(libraryZoom, 0, 0, false) : uiScale;
    }
}

StudioWindow::Impl::~Impl() {
    if (window) DestroyWindow(window);
    shutdown();
    DeleteObject(font);
    if (libraryFont) DeleteObject(libraryFont);
    DeleteObject(surfaceBrush);
    DeleteObject(fieldBrush);
    DeleteObject(activeBrush);
    if (trayIcon) DestroyIcon(trayIcon);
}

bool StudioWindow::Impl::create(HINSTANCE instance, int showCommand, bool startMinimized) {
    InitCommonControls();
    surfaceBrush = CreateSolidBrush(theme::surface);
    fieldBrush = CreateSolidBrush(theme::field);
    activeBrush = CreateSolidBrush(theme::active);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = windowProc;
    wc.lpszClassName = kBacktrackMainWindowClassName;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!registerWindowClass(wc)) return false;
    wc.lpfnWndProc = bodyProc;
    wc.lpszClassName = bodyClass;
    wc.hIcon = nullptr;
    if (!registerWindowClass(wc)) return false;
    dpi = GetDpiForSystem();
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, width = px(870), height = px(660);
    if (saved.windowWidth > 0) {
        const RECT candidate{saved.windowLeft, saved.windowTop,
                             saved.windowLeft + saved.windowWidth, saved.windowTop + saved.windowHeight};
        // A display that is gone must not leave the window off-screen.
        if (MonitorFromRect(&candidate, MONITOR_DEFAULTTONULL)) {
            x = saved.windowLeft;
            y = saved.windowTop;
            width = saved.windowWidth;
            height = saved.windowHeight;
        }
    }
    // Native frame: no maximize box, so no maximize button, snap maximize, or
    // double-click maximize to restore out of.
    if (!CreateWindowExW(0, kBacktrackMainWindowClassName, L"Backtrack",
                         (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) | WS_CLIPCHILDREN,
                         x, y, width, height, nullptr, nullptr, instance, this)) return false;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    Logger::instance().info(L"ui", std::wstring(L"Backtrack ") + std::wstring(kCurrentAppVersion));
    startSession();
    if (showCommand == SW_HIDE || showCommand == SW_SHOWDEFAULT || showCommand == SW_SHOWMINIMIZED ||
        showCommand == SW_SHOWMINNOACTIVE || showCommand == SW_MINIMIZE || showCommand == SW_SHOWMAXIMIZED)
        showCommand = SW_SHOWNORMAL;
    ShowWindow(window, startMinimized ? SW_SHOWMINIMIZED : showCommand);
    return true;
}

int StudioWindow::Impl::run() {
    MSG msg{};
    int result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (closing) {
            if ((msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) ||
                (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST)) continue;
        } else if (zoomInput(msg.message, msg.wParam) || handleShortcut(msg) || IsDialogMessageW(window, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return result < 0 ? 1 : static_cast<int>(msg.wParam);
}

LRESULT CALLBACK StudioWindow::Impl::windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (msg == WM_NCDESTROY && self) {
        self->window = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self ? self->message(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK StudioWindow::Impl::bodyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        if (!self) return FALSE;
        SetLastError(ERROR_SUCCESS);
        if (!SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)) && GetLastError() != ERROR_SUCCESS)
            return FALSE;
    }
    if (msg == WM_DESTROY && self && self->body == hwnd) self->closeLibrary();
    if (msg == WM_NCDESTROY && self) {
        if (self->body == hwnd) {
            self->body = nullptr;
            self->rows.clear();
            self->headingRects.clear();
            self->hotkeyFields.clear();
            self->invalidNumbers.clear();
            self->resolutionFields.fill(nullptr);
            self->deviceCombos = {};
            self->conditions.clear();
            self->visibility.clear();
            self->record = self->replayButton = self->recover = nullptr;
            self->setMotion(recordGlyph, self->motions[recordGlyph].to, false);
            std::fill(std::begin(self->diagnostics), std::end(self->diagnostics), nullptr);
            self->scroll = self->wheel = 0;
            self->pendingScroll.reset();
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self ? self->bodyMessage(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT StudioWindow::Impl::message(UINT msg, WPARAM wp, LPARAM lp) {
    if (const UINT activation = backtrackActivationMessage(); activation && msg == activation) {
        if (!closing) restore();
        return 0;
    }
    if (taskbarCreated && msg == taskbarCreated) {
        tray = false;
        if (!closing && !addTray() && !IsWindowVisible(window)) ShowWindow(window, SW_SHOWNA);
        return 0;
    }
    if (msg == WM_MOUSEWHEEL && zoomInput(msg, wp)) return 0;
    switch (msg) {
    case WM_CREATE: {
        updateMotionPreference();
        dpi = GetDpiForWindow(window);
        updateFont();
        for (size_t i = 0; i < pageButtons.size(); ++i) {
            pageButtons[i] = button(mainLabels[i], [this, i] {
                open(static_cast<Page>(i), clickOrigin == pageButtons[i]);
            }, L"", window);
            if (!pageButtons[i]) return -1;
            controls.at(pageButtons[i]).pointerMotion = true;
        }
        for (size_t i = 0; i < categoryButtons.size(); ++i) {
            categoryButtons[i] = button(settingsLabels[i], [this, i, category = static_cast<Category>(i)] {
                if (nav.page != Page::Settings || (nav.category == category && body)) return;
                if (auto invalid = invalidNumber()) {
                    SetFocus(invalid);
                    setStatus(L"Enter a valid number before changing settings categories.");
                    return;
                }
                const bool pointer = clickOrigin == categoryButtons[i];
                buildPage({nav.page, category});
                syncNav(pointer);
            }, L"", window);
            if (!categoryButtons[i]) return -1;
            controls.at(categoryButtons[i]).pointerMotion = true;
        }
        footer = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, {}, L"", window);
        if (!footer) return -1;
        setMotion(recordGlyph, static_cast<float>(controller.isRecording()), false);
        buildPage();
        if (!body) return -1;
        syncNav(false);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto& info = *reinterpret_cast<MINMAXINFO*>(lp);
        // DPI size, not the zoomed one: a large UI zoom must not leave the window
        // unshrinkable.
        info.ptMinTrackSize = {dpiPx(640), dpiPx(390)};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi = LOWORD(wp);
        updateFont();
        const auto& r = *reinterpret_cast<RECT*>(lp);
        SetWindowPos(window, nullptr, r.left, r.top, extent(r.left, r.right), extent(r.top, r.bottom), SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_ACTIVATE: {
        if (LOWORD(wp) == WA_INACTIVE) {
            const auto focus = GetFocus();
            if (focus && (focus == window || IsChild(window, focus))) lastFocus = focus;
        } else if (!closing && lastFocus && IsWindow(lastFocus) && IsWindowVisible(lastFocus)) {
            SetFocus(lastFocus);
        }
        return 0;
    }
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        // Windows terminates the process after this message, so the recording
        // has to be finalized and every worker joined here: the timer-driven
        // close path would never run.
        if (wp && !shuttingDown) {
            closing = true;
            EnableWindow(window, FALSE);
            shutdown();
            PostQuitMessage(0);
        }
        return 0;
    case WM_ENTERSIZEMOVE:
        sizing = true;
        stopMotion();
        return 0;
    case WM_EXITSIZEMOVE:
        sizing = false;
        if (!IsIconic(window)) layout();
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            actionFromPointer = false;
            stopMotion();
        } else if (sizing) InvalidateRect(window, nullptr, FALSE);
        else layout();
        return 0;
    case WM_SHOWWINDOW:
        if (!wp) {
            actionFromPointer = false;
            stopMotion();
        } else if (started && !closing) PostMessageW(window, WM_TIMER, diagnosticsTimer, 0);
        return 0;
    case WM_SETTINGCHANGE:
        updateMotionPreference();
        break;
    case WM_MOUSEWHEEL:
    case WM_XBUTTONUP:
        if (libraryInput(msg, wp)) return 0;
        break;
    case WM_ENABLE:
        if (!wp) stopMotion();
        break;
    case WM_PAINT: {
        // Lay out once for the size that will actually be painted during a drag.
        if (sizing && !IsIconic(window)) layout();
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(window, &ps);
        paintFrame(dc, ps.rcPaint);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT client{};
        GetClientRect(window, &client);
        paintFrame(reinterpret_cast<HDC>(wp), client);
        return 0;
    }
    case WM_ERASEBKGND: return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        auto dc = reinterpret_cast<HDC>(wp);
        auto hwnd = reinterpret_cast<HWND>(lp);
        wchar_t name[12]{};
        GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
        const auto cls = std::wstring_view(name);
        const bool field = cls == L"Edit" || cls == L"ComboLBox";
        const auto color = hwnd == footer ? theme::active : field ? theme::field : theme::surface;
        SetTextColor(dc, hwnd == footer ? theme::muted : IsWindowEnabled(hwnd) ? theme::text : theme::disabled);
        SetBkColor(dc, color);
        return reinterpret_cast<LRESULT>(hwnd == footer ? activeBrush : field ? fieldBrush : surfaceBrush);
    }
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item.CtlType != ODT_BUTTON && item.CtlType != ODT_COMBOBOX) break;
        const int dcState = SaveDC(item.hDC);
        if (item.CtlType == ODT_COMBOBOX) paintCombo(item);
        else if (!drawGallery(item)) paintButton(item);
        if (dcState) RestoreDC(item.hDC, dcState);
        return TRUE;
    }
    case WM_MEASUREITEM: {
        auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        item.itemHeight = px(22);
        return TRUE;
    }
    case WM_COMMAND: {
        if (building || closing || shuttingDown) return 0;
        auto hwnd = reinterpret_cast<HWND>(lp);
        auto control = controls.find(hwnd);
        if (control != controls.end() && control->second.event) {
            auto event = control->second.event;
            event(hwnd, HIWORD(wp));
        }
        return 0;
    }
    case syncHelp:
        focused = GetFocus();
        paintFooter();
        return 0;
    case WM_TIMER:
        if (wp == motionTimer) { tickMotion(); return 0; }
        if (wp == shutdownTimer) { finishClose(); return 0; }
        if (closing) return 0;
        if (wp == statusTimer) {
            KillTimer(window, statusTimer);
            if (!busy) setStatus({});
        } else if (wp == diagnosticsTimer) {
            completeDeviceDiscovery();
            if (IsWindowVisible(window) && !IsIconic(window)) updateDiagnostics();
        } else if (wp == autoRecordTimer) pollAutoRecord();
        return 0;
    case sessionStopped:
        if (closing) finishClose();
        else if (wp && actionWorkerDone.load()) {
            busy = false;
            applyingSettings = false;
            actionFromPointer = false;
            setMotion(recordGlyph, static_cast<float>(controller.isRecording()), false);
            if (record) setText(record, motions[recordGlyph].to != 0.f ? L"Stop recording" : L"Start recording");
            recoveryAvailable = false;
            syncRecoveryAction();
            setStatus(L"Recorder worker has stopped. Restart Backtrack to capture again.");
        }
        return 0;
    case actionReady: if (!closing) completeAction(); return 0;
    case galleryReady: if (!closing) galleryChanged(); return 0;
    case devicesReady:
        if (closing) finishClose();
        else completeDeviceDiscovery();
        return 0;
    case releaseReady:
        if (!closing && wp && !update) {
            addUpdateAction();
            RECT client{};
            if (update && GetClientRect(window, &client)) {
                layoutFooter(client);
                redrawWindowAndChildren(window);
            }
        }
        return 0;
    case voiceReplay:
        if (!closing && !applyingSettings && voiceEvents.load() && voice.stats().ready) requestReplay();
        return 0;
    case leagueKill:
        if (!closing && !applyingSettings && leagueEvents.load()) {
            killDeadline = SteadyClock::now() + std::chrono::seconds(6);
            playActionIndicator(MB_ICONASTERISK, saved.notificationSoundVolumePercent);
            setStatus(L"Kill detected. Save a replay within 6 seconds to tag it.");
        }
        return 0;
    case WM_HOTKEY: {
        const bool replay = wp == HotkeyService::kSaveReplayId;
        if (closing || (!replay && wp != HotkeyService::kStartStopId) ||
            !hotkeys.shouldHandleHotkeyMessage(static_cast<int>(wp), lp)) return 0;
        const auto focus = GetFocus();
        if (hotkeyFields.contains(focus)) {
            const auto& keys = saved.hotkeys;
            const uint32_t modifiers = lp == hookHotkey
                ? (replay ? keys.saveReplayModifiers : keys.startStopModifiers) : LOWORD(lp);
            const uint32_t key = lp == hookHotkey
                ? (replay ? keys.saveReplayVirtualKey : keys.startStopVirtualKey) : HIWORD(lp);
            if (key) editHotkey(focus, modifiers, key);
        } else if (replay) requestReplay();
        else submit(ControllerActionKind::ToggleRecording, L"Changing recording state\u2026");
        return 0;
    }
    case trayEvent:
        if (closing) return 0;
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) restore();
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) trayMenu();
        return 0;
    case WM_CLOSE: close(false); return 0;
    case WM_DESTROY:
        stopMotion();
        shutdown();
        if (started) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

LRESULT StudioWindow::Impl::bodyMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return TRUE;
    case WM_PAINT: {
        if (pendingScroll) scrollTo(*pendingScroll);
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(hwnd, &ps);
        theme::fill(dc, ps.rcPaint, theme::surface);
        SelectObject(dc, font);
        SetTextColor(dc, theme::text);
        SetBkMode(dc, TRANSPARENT);
        for (size_t i = 0; i < rows.size() && i < headingRects.size(); ++i) {
            RECT r = headingRects[i], invalid{};
            if (!IntersectRect(&invalid, &r, &ps.rcPaint)) continue;
            const auto& item = rows[i];
            for (int depth = 0; depth < item.dependencyDepth; ++depth) {
                const int x = r.left + px(item.inset + depth * theme::dependencyIndent);
                theme::fill(dc, {x, r.top, x + px(1), r.bottom}, theme::rule);
            }
            const auto& title = item.heading;
            if (title.empty()) continue;
            SIZE size{};
            GetTextExtentPoint32W(dc, title.c_str(), static_cast<int>(title.size()), &size);
            const int center = (r.top + r.bottom) / 2, line = center - 1;
            const int x = r.left + px(theme::sectionInset);
            theme::fill(dc, {r.left, line, x - px(7), line + 1}, theme::divider);
            ExtTextOutW(dc, x, center - size.cy / 2, ETO_CLIPPED, &r, title.c_str(), static_cast<UINT>(title.size()), nullptr);
            theme::fill(dc, {x + size.cx + px(7), line, r.right, line + 1}, theme::divider);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_VSCROLL: {
        if (hwnd != body || lp) break;
        SCROLLINFO info{sizeof(info), SIF_ALL};
        if (!GetScrollInfo(hwnd, SB_VERT, &info)) break;
        int y = pendingScroll.value_or(scroll);
        switch (LOWORD(wp)) {
        case SB_LINEUP: y -= px(theme::rowHeight); break;
        case SB_LINEDOWN: y += px(theme::rowHeight); break;
        case SB_PAGEUP: y -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: y += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: y = info.nTrackPos; break;
        case SB_TOP: y = 0; break;
        case SB_BOTTOM: y = info.nMax; break;
        default: return 0;
        }
        scrollTo(y);
        return 0;
    }
    case WM_XBUTTONUP:
        if (hwnd == body && libraryInput(msg, wp)) return 0;
        break;
    case WM_MOUSEWHEEL: {
        if (hwnd != body) break;
        // The library scrolls its gallery wherever the wheel is on the page.
        if (libraryInput(msg, wp)) return 0;
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        RECT r{};
        GetClientRect(hwnd, &r);
        wheel += GET_WHEEL_DELTA_WPARAM(wp);
        const int steps = wheel / WHEEL_DELTA;
        wheel %= WHEEL_DELTA;
        const int distance = lines == WHEEL_PAGESCROLL ? r.bottom : px(16) * static_cast<int>((std::min)(lines, 100U));
        if (steps && distance) scrollTo(pendingScroll.value_or(scroll) - steps * distance, true);
        return 0;
    }
    case WM_COMMAND: case WM_DRAWITEM: case WM_MEASUREITEM:
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        return message(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND StudioWindow::Impl::make(const wchar_t* type, const wchar_t* label, DWORD style, ControlEvent callback,
                              const wchar_t* help, HWND parent) {
    const HWND hwnd = CreateWindowExW(0, type, label, WS_CHILD | WS_CLIPSIBLINGS | style, 0, 0, 0, 0,
                                     parent ? parent : body, nullptr, nullptr, nullptr);
    if (!hwnd) return nullptr;
    controls.emplace(hwnd, Control{std::move(callback), help});
    const bool chrome = (parent ? parent : body) == window;
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(chrome ? font : pageFont()), FALSE);
    SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
    SetWindowSubclass(hwnd, controlProc, 1, reinterpret_cast<DWORD_PTR>(this));
    return hwnd;
}

HWND StudioWindow::Impl::button(const wchar_t* label, Action click, const wchar_t* help, HWND parent, ButtonTone tone) {
    const auto hwnd = make(L"BUTTON", label, BS_OWNERDRAW | WS_VISIBLE | WS_TABSTOP,
                           [click = std::move(click)](HWND, UINT code) { if (code == BN_CLICKED) click(); }, help, parent);
    if (hwnd) controls.at(hwnd).tone = tone;
    return hwnd;
}


LRESULT CALLBACK StudioWindow::Impl::controlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto& self = *reinterpret_cast<Impl*>(data);
    const auto it = self.controls.find(hwnd);
    if (it == self.controls.end()) return DefSubclassProc(hwnd, msg, wp, lp);
    const bool check = it->second.checkbox;
    switch (msg) {
    case WM_MOUSEMOVE:
        if (self.hovered != hwnd) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            self.setHovered(hwnd);
        }
        break;
    case WM_MOUSELEAVE:
        if (self.hovered == hwnd) self.setHovered(nullptr);
        break;
    case BM_CLICK:
    case WM_LBUTTONUP: {
        if (!it->second.pointerMotion || (msg == WM_LBUTTONUP && self.clickOrigin == nullptr)) break;
        // An engaged null origin suppresses mouse messages synthesized by BM_CLICK.
        const auto previous = std::exchange(self.clickOrigin, std::optional<HWND>{msg == WM_LBUTTONUP ? hwnd : nullptr});
        const auto result = DefSubclassProc(hwnd, msg, wp, lp);
        self.clickOrigin = previous;
        return result;
    }
    case WM_XBUTTONUP:
        if (self.libraryInput(msg, wp)) return 0;
        break;
    case WM_SETFOCUS:
        self.ensureVisible(hwnd);
        [[fallthrough]];
    case WM_KILLFOCUS:
        PostMessageW(self.window, syncHelp, 0, 0);
        if (check) InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_MOUSEWHEEL:
        if (!it->second.combo || !SendMessageW(hwnd, CB_GETDROPPEDSTATE, 0, 0)) {
            if (self.libraryInput(msg, wp)) return 0;
            const bool library = self.nav.page == Page::Library;
            auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            if (!library && GetParent(hwnd) == self.body && !(style & WS_VSCROLL && style & ES_MULTILINE))
                return SendMessageW(self.body, msg, wp, lp);
            if (hwnd == self.footer) return DefSubclassProc(hwnd, msg, wp, lp);
        }
        break;
    case WM_GETDLGCODE:
        if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & ES_MULTILINE) != 0) {
            wchar_t cls[8]{};
            GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
            if (std::wstring_view(cls) == L"Edit") return DefSubclassProc(hwnd, msg, wp, lp) & ~DLGC_WANTALLKEYS;
        }
        break;
    case WM_PAINT:
        if (check) {
            PAINTSTRUCT ps{};
            const auto dc = BeginPaint(hwnd, &ps);
            self.paintCheckbox(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    case WM_PRINTCLIENT:
        if (check) { self.paintCheckbox(hwnd, reinterpret_cast<HDC>(wp)); return 0; }
        break;
    case WM_ERASEBKGND:
        if (check || (GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW) return TRUE;
        break;
    case WM_NCDESTROY:
        if (self.hovered == hwnd) self.hovered = nullptr;
        if (self.focused == hwnd) self.focused = nullptr;
        if (self.lastFocus == hwnd) self.lastFocus = nullptr;
        if (self.save == hwnd) self.save = nullptr;
        self.controls.erase(it);
        RemoveWindowSubclass(hwnd, controlProc, id);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void StudioWindow::Impl::open(Page page, bool pointer) {
    if (nav.page == page && body) {
        // Library is also the way out of an open folder.
        if (page == Page::Library) libraryToRoot();
        return;
    }
    if (nav.page != page && !leaveSettings(false)) return;
    buildPage({page, nav.category});
    syncNav(pointer);
    SetFocus(nav.page == Page::Settings ? categoryButtons[static_cast<size_t>(nav.category)] : pageButtons[static_cast<size_t>(nav.page)]);
}

void StudioWindow::Impl::syncNav(bool pointer) {
    setMotion(mainMarker, static_cast<float>(nav.page), pointer);
    for (auto tab : pageButtons) InvalidateRect(tab, nullptr, FALSE);
    const bool settings = nav.page == Page::Settings;
    const bool wasSettings = (GetWindowLongPtrW(categoryButtons.front(), GWL_STYLE) & WS_VISIBLE) != 0;
    setMotion(categoryMarker, static_cast<float>(nav.category), pointer && settings && wasSettings);
    for (auto tab : categoryButtons) {
        if (settings != wasSettings) ShowWindow(tab, settings ? SW_SHOWNA : SW_HIDE);
        if (settings) InvalidateRect(tab, nullptr, FALSE);
    }
}

void StudioWindow::Impl::setHovered(HWND control) {
    if (const auto old = std::exchange(hovered, control)) InvalidateRect(old, nullptr, FALSE);
    if (control) InvalidateRect(control, nullptr, FALSE);
    paintFooter();
}

void StudioWindow::Impl::buildPage() { buildPage(nav); }

void StudioWindow::Impl::buildPage(StudioNavigation next) {
    if (building || shuttingDown) return;
    const bool enteringAudio = next.page == Page::Settings && next.category == Category::Audio &&
        (!body || nav.page != next.page || nav.category != next.category);
    building = true;
    if (body && !DestroyWindow(body)) {
        building = false;
        setStatus(L"Could not close the current page.");
        return;
    }
    assert(!body);
    if (save && GetParent(save) == window) DestroyWindow(save);
    hovered = focused = save = nullptr;
    nav = next;
    const bool scrollable = nav.page == Page::Capture || nav.page == Page::Settings;
    // A scrolled frame is only complete once the viewport and its HWNDs have painted.
    const DWORD buffering = scrollable || nav.page == Page::Library ? WS_EX_COMPOSITED : 0;
    body = CreateWindowExW(WS_EX_CONTROLPARENT | buffering, bodyClass, nullptr,
        WS_CHILD | WS_CLIPCHILDREN | (scrollable ? WS_VSCROLL : 0), 0, 0, 0, 0, window, nullptr,
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE)), this);
    if (body) {
        SetWindowTheme(body, L"DarkMode_Explorer", nullptr);
        if (enteringAudio) startDeviceDiscovery();
        switch (nav.page) {
        case Page::Capture: buildCapture(); break;
        case Page::Library: buildLibrary(); break;
        case Page::Settings: buildSettings(); break;
        case Page::Diagnostics: buildDiagnostics(); break;
        }
    }
    layout(true);
    if (body) ShowWindow(body, SW_SHOWNA);
    building = false;
    if (!body) {
        if (started) setStatus(L"Could not open this page. Select its tab to retry.");
        return;
    }
    changed();
    paintFooter();
    if (nav.page == Page::Diagnostics && started) updateDiagnostics();
}

void StudioWindow::Impl::updateFont() {
    // GDI maps the base Bahnschrift face to regular even at weight 350.
    const auto make = [this](int height) {
        return CreateFontW(-height, 0, 0, 0, 350, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Bahnschrift SemiLight");
    };
    const auto previousFont = std::exchange(font, make(px(15)));
    if (libraryFont) {
        DeleteObject(libraryFont);
        libraryFont = nullptr;
    }
    if (galleryScale != uiScale) libraryFont = make(libraryPx(15));
    for (const auto& [hwnd, control] : controls) {
        // Chrome keeps the app font; the library page's own controls follow its
        // scale.
        const bool chrome = GetParent(hwnd) == window;
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(chrome ? font : pageFont()), TRUE);
        // Edit margins are in pixels, so they need the new DPI scale.
        if (control.margin)
            SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(pagePx(control.margin), pagePx(control.margin)));
        if (control.combo) {
            SendMessageW(hwnd, CB_SETITEMHEIGHT, -1, px(21));
            SendMessageW(hwnd, CB_SETITEMHEIGHT, 0, px(22));
        }
    }
    DeleteObject(previousFont);
}

void StudioWindow::Impl::saveUiState() {
    if (!window || !readable) return;
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(window, &placement)) return;
    const auto& normal = placement.rcNormalPosition;
    auto updated = saved;
    updated.windowLeft = normal.left;
    updated.windowTop = normal.top;
    updated.windowWidth = normal.right - normal.left;
    updated.windowHeight = normal.bottom - normal.top;
    // The window has no maximize box, so the stored flag stays clear.
    updated.windowMaximized = false;
    updated.uiZoomPercent = static_cast<uint32_t>(uiZoom);
    updated.libraryZoomPercent = static_cast<uint32_t>(libraryZoom);
    if (updated == saved) return;
    // Placement and zoom are not draft fields: publish the saved baseline with
    // them, never the unsaved page edits, and keep the draft in step so a pending
    // edit stays the only dirty difference.
    if (!store.save(updated)) return;
    saved = std::move(updated);
    draft.windowLeft = saved.windowLeft;
    draft.windowTop = saved.windowTop;
    draft.windowWidth = saved.windowWidth;
    draft.windowHeight = saved.windowHeight;
    draft.windowMaximized = saved.windowMaximized;
    draft.uiZoomPercent = saved.uiZoomPercent;
    draft.libraryZoomPercent = saved.libraryZoomPercent;
}

bool StudioWindow::Impl::keyboardCuesHidden() const {
    return (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
}

void StudioWindow::Impl::layout(bool newPage) {
    if (!footer || shuttingDown) return;
    RECT rect{};
    if (!GetClientRect(window, &rect)) return;
    // Rescale before measuring anything: the default window width is the 100%
    // anchor, so the layout keeps its density at the size the app opens with.
    const int scale = studio_theme::uiScalePercent(uiZoom, rect.right, dpiPx(870), saved.uiScaleWithWindow);
    const int libraryScale = studio_theme::uiScalePercent(saved.libraryOwnScale ? libraryZoom : uiZoom,
                                                          rect.right, dpiPx(870), saved.uiScaleWithWindow);
    if (scale != uiScale || libraryScale != galleryScale) {
        uiScale = scale;
        galleryScale = libraryScale;
        updateFont();
    }
    const bool tabsChanged = rect.right != layoutSize.cx || dpi != layoutDpi || scale != layoutScale ||
        libraryScale != layoutLibraryScale;
    if (!newPage && !tabsChanged && rect.bottom == layoutSize.cy) return;
    layoutSize = {rect.right, rect.bottom};
    layoutDpi = dpi;
    layoutScale = scale;
    layoutLibraryScale = libraryScale;

    const int width = static_cast<int>(rect.right);
    const int navHeight = px(theme::navHeight);
    const int tabHeight = navHeight - px(theme::navUnderlineBand);
    if (tabsChanged) {
        std::vector<WindowMove> moves;
        moves.reserve(pageButtons.size() + categoryButtons.size());
        const auto placeTabs = [&](const auto& buttons, int y) {
            const int count = static_cast<int>(buttons.size());
            const RECT row{0, y, width, y + tabHeight};
            for (int i = 0; i < count; ++i) {
                const auto tab = theme::tabBounds(row, i, count, px(theme::gap));
                moves.push_back({buttons[i], tab.left, y, tab.right - tab.left, tabHeight});
            }
        };
        placeTabs(pageButtons, 0);
        placeTabs(categoryButtons, navHeight);
        applyWindowMoves(moves);
    }
    const int bodyTop = (nav.page == Page::Settings ? 2 : 1) * navHeight;
    if (body) {
        moveWindowIfChanged(body, 0, bodyTop, width,
                            (std::max)(0, static_cast<int>(rect.bottom) - bodyTop - px(theme::footerHeight)));
        if (nav.page == Page::Library) layoutGallery();
        else if (nav.page == Page::Diagnostics) layoutDiagnostics();
        // layout() repaints the window and all children once it has positioned
        // everything, so the row pass must not queue its own.
        else layoutRows(!newPage && !tabsChanged, false);
    }
    layoutFooter(rect);
    redrawWindowAndChildren(window);
}

void StudioWindow::Impl::layoutFooter(const RECT& client) {
    const int inset = px(theme::inset), footerHeight = px(theme::footerHeight);
    const int footerTop = client.bottom - footerHeight;
    int footerLeft = inset, footerRight = client.right - inset;
    const int actionHeight = px(28), actionTop = footerTop + (footerHeight - actionHeight) / 2;
    if (save) {
        const int saveWidth = px(nav.page == Page::Settings && nav.category == Category::Capture ? 80 : 116);
        moveWindowIfChanged(save, footerLeft, actionTop, saveWidth, actionHeight);
        footerLeft += saveWidth + px(theme::gap);
    }
    if (update) {
        const int updateWidth = px(132);
        moveWindowIfChanged(update, footerRight - updateWidth, actionTop, updateWidth, actionHeight);
        footerRight -= updateWidth + px(theme::gap);
    }
    const int textTop = footerTop + px(5), bottomGap = px(3);
    replayCheckBox = {footerLeft, textTop, footerLeft + px(26), textTop + px(22)};
    footerLeft = replayCheckBox.right;
    moveWindowIfChanged(footer, footerLeft, textTop, (std::max)(0, footerRight - footerLeft), client.bottom - bottomGap - textTop);
}

void StudioWindow::Impl::row(std::initializer_list<HWND> controls, bool split, int height) {
    rows.push_back({controls, {}, height ? height : theme::rowHeight, split});
}

void StudioWindow::Impl::section(const wchar_t* title) {
    rows.push_back({{}, title, 38});
}

void StudioWindow::Impl::layoutRows(bool heightOnly, bool repaint) {
    if (!body || arranging) return;
    arranging = true;
    RECT r{};
    GetClientRect(body, &r);
    int content = px(8);
    for (const auto& item : rows) {
        if (!item.hidden) content += px(item.height);
    }
    const int previousScroll = scroll;
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, content - 1,
                    static_cast<UINT>(r.bottom), scroll};
    scroll = SetScrollInfo(body, SB_VERT, &info, FALSE);
    // The scrollbar can appear or disappear, changing the usable width.
    GetClientRect(body, &r);
    if (heightOnly && r.right == rowWidth && scroll == previousScroll) {
        arranging = false;
        return;
    }
    rowWidth = r.right;
    const int left = px(theme::inset), right = (std::max)(left, static_cast<int>(r.right) - left);
    const int gap = px(theme::gap);
    int y = px(3) - scroll;
    std::vector<WindowMove> moves;
    moves.reserve(controls.size());
    headingRects.resize(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& item = rows[i];
        const int height = item.hidden ? 0 : px(item.height);
        headingRects[i] = {left, y, right, y + height};
        if (!height) continue;
        int x = left + px(item.inset + item.dependencyDepth * theme::dependencyIndent);
        for (size_t j = 0; j < item.cells.size(); ++j) {
            const auto cell = item.cells[j];
            const int remaining = static_cast<int>(item.cells.size() - j);
            int w = remaining == 1 ? right - x : (right - x - gap * (remaining - 1)) / remaining;
            if (item.widths[0]) {
                const int tail = item.tail && remaining == 2 ? px(item.widths[j + 1]) : 0;
                w = (std::min)(px(item.widths[j]), right - x - gap * (remaining - 1) - tail);
            } else if (item.split && j == 0 && remaining > 1) w = (right - left) * 36 / 100;
            else if (item.tail && remaining == 2) w = right - x - gap - px(88);
            w = (std::max)(1, w);
            if (cell) {
                const auto& control = controls.at(cell);
                moves.push_back({cell, x, y + px(3), w, control.combo ? px(222) : height - px(6)});
            }
            x += w + gap;
        }
        y += height;
    }
    applyWindowMoves(moves);
    if (repaint) redrawWindowAndChildren(body);
    arranging = false;
}

void StudioWindow::Impl::scrollTo(int value, bool onPaint) {
    if (!body || arranging || (nav.page != Page::Capture && nav.page != Page::Settings)) return;
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE};
    if (!GetScrollInfo(body, SB_VERT, &info)) return;
    const int last = info.nMax - (std::max)(0, static_cast<int>(info.nPage) - 1);
    value = std::clamp(value, info.nMin, last);
    if (onPaint) {
        if (value != pendingScroll.value_or(scroll)) {
            pendingScroll = value;
            InvalidateRect(body, nullptr, FALSE);
        }
        return;
    }
    pendingScroll.reset();
    if (value == scroll) return;
    scroll = value;
    layoutRows();
}

void StudioWindow::Impl::ensureVisible(HWND control) {
    if (building || !body || nav.page == Page::Library || GetParent(control) != body) return;
    RECT view{}, r{};
    GetClientRect(body, &view);
    GetWindowRect(control, &r);
    MapWindowPoints(nullptr, body, reinterpret_cast<POINT*>(&r), 2);
    const auto pad = px(3);
    if (r.top < pad) scrollTo(scroll + r.top - pad);
    else if (r.bottom + pad > view.bottom && r.bottom - r.top < view.bottom) scrollTo(scroll + r.bottom + pad - view.bottom);
}

void StudioWindow::Impl::paintFrame(HDC dc, const RECT& dirty) {
    theme::fill(dc, dirty, theme::surface);
    RECT r{};
    GetClientRect(window, &r);
    paintTabMarker(dc, mainMarker);
    if (nav.page == Page::Settings) paintTabMarker(dc, categoryMarker);
    r.top = r.bottom - px(theme::footerHeight);
    if (dirty.bottom > r.top && IsWindowVisible(footer)) {
        theme::fill(dc, r, theme::active);
        theme::fill(dc, {r.left, r.top, r.right, r.top + 1}, theme::rule);
        paintReplayCheck(dc);
    }
}

void StudioWindow::Impl::paintButton(const DRAWITEMSTRUCT& item) {
    const auto found = controls.find(item.hwndItem);
    // Chrome keeps the app scale even while the library page carries its own.
    const bool chrome = GetParent(item.hwndItem) == window;
    const auto inset = [&](int dip) { return chrome ? px(dip) : pagePx(dip); };
    auto dc = item.hDC;
    auto r = item.rcItem;
    const auto page = std::find(pageButtons.begin(), pageButtons.end(), item.hwndItem);
    const auto category = std::find(categoryButtons.begin(), categoryButtons.end(), item.hwndItem);
    const bool navigation = page != pageButtons.end() || category != categoryButtons.end();
    const bool selected = page == pageButtons.begin() + static_cast<size_t>(nav.page) ||
        (nav.page == Page::Settings && category == categoryButtons.begin() + static_cast<size_t>(nav.category));
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool down = (item.itemState & ODS_SELECTED) != 0;
    const bool hot = !disabled && hovered == item.hwndItem;
    const auto tone = found == controls.end() ? ButtonTone::Normal : found->second.tone;
    const bool quiet = tone == ButtonTone::Quiet;
    const auto base = selected || tone == ButtonTone::Primary ? theme::active : navigation || quiet ? theme::surface : theme::field;
    theme::fill(dc, r, !navigation && down ? theme::press : !navigation && hot ? theme::hover : base);
    // Windows reports a suppressed focus rectangle through ODS_NOFOCUSRECT
    // (pointer-driven focus, or cues hidden); the theme outline follows the same
    // rule as the native one.
    const bool focusCue = (item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS;
    if (navigation) {
        if (focusCue) {
            RECT focus = r;
            InflateRect(&focus, -inset(3), -inset(1));
            theme::frame(dc, focus, theme::accent);
        }
        r.bottom += inset(theme::navUnderlineBand);
    } else {
        if (!quiet) theme::frame(dc, r, disabled ? theme::rule : theme::border);
        if (focusCue) {
            RECT focus = r;
            InflateRect(&focus, -inset(3), -inset(3));
            theme::frame(dc, focus, theme::accent);
        }
    }
    if (item.hwndItem == record) {
        const auto box = recordingGlyphBox(r);
        paintRecordingGlyph(dc, box, disabled);
        r.left = box.right;
    }
    InflateRect(&r, -inset(6), 0);
    if (!RectVisible(dc, &r)) return;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? theme::disabled : quiet && !hot ? theme::muted : theme::text);
    SelectObject(dc, chrome ? font : pageFont());
    const auto label = readText(item.hwndItem);
    DrawTextW(dc, label.c_str(), -1, &r, (quiet ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void StudioWindow::Impl::paintCheckbox(HWND hwnd, HDC dc) {
    RECT r{};
    GetClientRect(hwnd, &r);
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const bool checked = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool down = (SendMessageW(hwnd, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
    theme::fill(dc, r, theme::surface);
    const int size = px(15), x = px(1), y = (r.bottom - size) / 2;
    const RECT box{x, y, x + size, y + size};
    const auto checkedFill = enabled ? theme::muted : theme::disabled;
    theme::fill(dc, box, checked ? checkedFill : down ? theme::press : hovered == hwnd ? theme::hover : theme::field);
    theme::frame(dc, box, checked ? checkedFill : enabled ? theme::border : theme::rule);
    if (checked) {
        const auto pen = CreatePen(PS_SOLID, px(2), theme::surface);
        const auto old = SelectObject(dc, pen);
        const POINT points[]{{x + px(3), y + px(8)}, {x + px(6), y + px(11)}, {x + px(12), y + px(4)}};
        Polyline(dc, points, 3);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    if (GetFocus() == hwnd && !keyboardCuesHidden()) theme::frame(dc, r, theme::accent);
    r.left += px(24);
    if (!RectVisible(dc, &r)) return;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? theme::text : theme::disabled);
    SelectObject(dc, font);
    const auto label = readText(hwnd);
    DrawTextW(dc, label.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void StudioWindow::Impl::paintCombo(const DRAWITEMSTRUCT& item) {
    auto r = item.rcItem;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    theme::fill(item.hDC, r, selected ? theme::active : theme::field);
    if (item.itemID == static_cast<UINT>(-1)) return;
    const auto length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
    if (length == CB_ERR) return;
    std::wstring label(static_cast<size_t>(length) + 1, L'\0');
    SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(label.data()));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, (item.itemState & ODS_DISABLED) ? theme::disabled : theme::text);
    SelectObject(item.hDC, font);
    if ((item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS) theme::frame(item.hDC, r, theme::accent);
    InflateRect(&r, -px(6), 0);
    DrawTextW(item.hDC, label.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void StudioWindow::Impl::setStatus(std::wstring value, bool savedReplay, bool pointer) {
    status = std::move(value);
    replaySaved = savedReplay;
    if (!status.empty()) SetTimer(window, statusTimer, 9000, nullptr);
    paintFooter(pointer);
}

void StudioWindow::Impl::paintFooter(bool animateCheck) {
    if (!footer || building || shuttingDown || focused == footer) return;
    auto help = [this](HWND hwnd) -> std::wstring_view {
        const auto it = controls.find(hwnd);
        return it != controls.end() && IsWindowVisible(hwnd) ? it->second.help : std::wstring_view{};
    };
    auto hint = help(hovered);
    if (hint.empty()) hint = help(focused);
    std::wstring text = status;
    const auto append = [&text](std::wstring_view line) {
        if (line.empty()) return;
        if (!text.empty()) text += L"\r\n";
        text.append(line);
    };
    append(hint);
    append(persistentWarning);
    append(audioWarning);
    // The zoom gesture has no control of its own, so an idle footer names it
    // without crowding a status or a hover hint.
    if (text.empty()) text = L"Ctrl+Scroll scales the font";
    if (motions[replayCheck].to != static_cast<float>(replaySaved))
        setMotion(replayCheck, static_cast<float>(replaySaved), replaySaved && animateCheck);
    const bool showing = (GetWindowLongPtrW(footer, GWL_STYLE) & WS_VISIBLE) != 0;
    if (text.empty()) {
        if (!showing) return;
        ShowWindow(footer, SW_HIDE);
    } else {
        setText(footer, text);
        if (showing) return;
        ShowWindow(footer, SW_SHOWNA);
    }
    RECT r{};
    GetClientRect(window, &r);
    r.top = r.bottom - px(theme::footerHeight);
    InvalidateRect(window, &r, FALSE);
}

bool StudioWindow::Impl::editHotkey(HWND field, uint32_t modifiers, uint32_t key) {
    const auto it = hotkeyFields.find(field);
    if (it == hotkeyFields.end()) return false;
    if (isModifierKey(key)) return true;
    if (key == VK_DELETE || key == VK_BACK) modifiers = key = 0;
    modifiers = supportedHotkeyModifiers(modifiers);
    if (!isSafeGlobalHotkey(modifiers, key)) { setStatus(L"Hotkeys for normal keys need Ctrl or Alt"); return true; }
    const bool startStop = it->second.first == &draft.hotkeys.startStopModifiers;
    const uint32_t otherModifiers = startStop ? draft.hotkeys.saveReplayModifiers : draft.hotkeys.startStopModifiers;
    const uint32_t otherKey = startStop ? draft.hotkeys.saveReplayVirtualKey : draft.hotkeys.startStopVirtualKey;
    // Letting sanitizeSettings resolve a duplicate would silently reset the
    // replay binding; refuse the edit while both fields are still on screen.
    if (key && key == otherKey && modifiers == otherModifiers) {
        setStatus(startStop ? L"That shortcut already saves a replay." : L"That shortcut already starts and stops recording.");
        return true;
    }
    *it->second.first = modifiers;
    *it->second.second = key;
    setText(field, hotkeyDisplay(modifiers, key));
    changed();
    return true;
}

bool StudioWindow::Impl::zoomInput(UINT message, WPARAM wheel) {
    // Ctrl+Scroll is taken in the message loop, so it rescales from any page and
    // over any child control instead of reaching that control's own wheel path.
    // The modifier comes from the message: it is the state the wheel was turned
    // with, and a test can send it. `message` also asks, so a wheel message that
    // reaches the window directly still zooms.
    if (message != WM_MOUSEWHEEL || (GET_KEYSTATE_WPARAM(wheel) & MK_CONTROL) == 0) return false;
    const int delta = GET_WHEEL_DELTA_WPARAM(wheel);
    if (!delta) return true;
    // The gesture scales whatever the current page shows: the library carries its
    // own zoom while the separate scale is on.
    const bool libraryPage = nav.page == Page::Library && saved.libraryOwnScale;
    int& current = libraryPage ? libraryZoom : uiZoom;
    const int zoom = std::clamp(current + (delta > 0 ? 10 : -10), theme::minUiZoom, theme::maxUiZoom);
    if (zoom != current) {
        current = zoom;
        layout();
        // Persist on the notch, not on exit: closing to the tray or an abnormal
        // end would otherwise drop the scale the user just set.
        saveUiState();
    }
    return true;
}

bool StudioWindow::Impl::handleShortcut(const MSG& msg) {
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
    const HWND focus = GetFocus();
    if (msg.wParam != VK_TAB && msg.wParam != VK_ESCAPE &&
        editHotkey(focus, currentModifierState(), static_cast<uint32_t>(msg.wParam))) return true;
    if (nav.page == Page::Library && galleryShortcut(msg)) return true;
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && controls.contains(focus) && IsWindowEnabled(focus)) {
        wchar_t cls[8]{};
        GetClassNameW(focus, cls, static_cast<int>(std::size(cls)));
        if (std::wstring_view(cls) == L"Button" && (GetWindowLongPtrW(focus, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW) {
            if (!(msg.lParam & (1LL << 30))) SendMessageW(focus, BM_CLICK, 0, 0);
            return true;
        }
    }
    return false;
}

} // namespace backtrack
