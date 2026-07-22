#include "ui/MainWindow.h"
#include "ui/UiHelpers.h"
#include "ui/UiIds.h"

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
