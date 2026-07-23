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
            const HWND underCursor = reinterpret_cast<HWND>(wParam);
            self->updateStatusHelp(underCursor);
            self->updateButtonHover(underCursor);
        }
        break;
    case WM_MOUSEMOVE:
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            const HWND underCursor = WindowFromPoint(cursor);
            self->updateStatusHelp(underCursor);
            self->updateButtonHover(underCursor);
        }
        break;
    case WM_MOUSELEAVE:
        self->clearButtonHover();
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

LRESULT CALLBACK MainWindow::comboBoxSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData) {
    auto* self = reinterpret_cast<MainWindow*>(refData);
    switch (message) {
    case WM_MOUSEWHEEL:
        if (self && SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0) == FALSE) {
            self->scrollPageWheel(wParam);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, comboBoxSubclassProc, subclassId);
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
            const HWND underCursor = reinterpret_cast<HWND>(wParam);
            updateStatusHelp(underCursor);
            updateButtonHover(underCursor);
        }
        break;
    case WM_MOUSEMOVE:
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            const HWND underCursor = WindowFromPoint(cursor);
            updateStatusHelp(underCursor);
            updateButtonHover(underCursor);
        }
        break;
    case WM_MOUSELEAVE:
        clearButtonHover();
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
        if (settingsDirty_ && !promptSaveSettingsIfDirty(true)) {
            return 0;
        }
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
    const bool hovered = !pressed && !disabled && item.hwndItem == hoveredButton_;
    const COLORREF textColor = disabled ? kMutedText : kText;

    HBRUSH brush = controlBrush_;
    if (pressed) {
        brush = buttonPressedBrush_ ? buttonPressedBrush_ : selectionBrush_;
    } else if (selected) {
        brush = tabActiveBrush_ ? tabActiveBrush_ : selectionBrush_;
    } else if (hovered) {
        brush = buttonHoverBrush_ ? buttonHoverBrush_ : controlBrush_;
    }
    FillRect(item.hDC, &item.rcItem, brush);

    HPEN outline = selected
        ? (tabActiveOutlinePen_ ? tabActiveOutlinePen_ : selectedOutlinePen_)
        : outlinePen_;
    HGDIOBJ oldPen = SelectObject(item.hDC, outline);
    HGDIOBJ oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
    if (selectedTab) {
        const int underlineTop = item.rcItem.bottom - 3;
        HBRUSH underline = selectionBrush_;
        RECT bar{item.rcItem.left + 2, underlineTop, item.rcItem.right - 2, item.rcItem.bottom - 1};
        FillRect(item.hDC, &bar, underline);
    }
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
    const HFONT font = selected && headingFont_ ? headingFont_ : font_;
    HGDIOBJ oldFont = font ? SelectObject(item.hDC, font) : nullptr;
    DrawTextW(item.hDC, textBuffer, -1, &textRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
    if (oldFont) {
        SelectObject(item.hDC, oldFont);
    }

    if ((item.itemState & ODS_FOCUS) != 0) {
        DrawFocusRect(item.hDC, &item.rcItem);
    }
}

void MainWindow::updateButtonHover(HWND control) {
    HWND button = control;
    while (button && button != window_ && button != pageHost_) {
        wchar_t className[32]{};
        if (GetClassNameW(button, className, static_cast<int>(_countof(className))) > 0 &&
            lstrcmpW(className, L"Button") == 0) {
            break;
        }
        button = GetParent(button);
    }
    if (!button || button == window_ || button == pageHost_) {
        clearButtonHover();
        return;
    }

    if (hoveredButton_ == button) {
        if (!buttonHoverTracking_) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = window_;
            TrackMouseEvent(&track);
            buttonHoverTracking_ = true;
        }
        return;
    }

    HWND previous = hoveredButton_;
    hoveredButton_ = button;
    if (previous) {
        InvalidateRect(previous, nullptr, FALSE);
    }
    InvalidateRect(hoveredButton_, nullptr, FALSE);

    TRACKMOUSEEVENT track{};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = window_;
    TrackMouseEvent(&track);
    buttonHoverTracking_ = true;
}

void MainWindow::clearButtonHover() {
    buttonHoverTracking_ = false;
    if (!hoveredButton_) {
        return;
    }
    HWND previous = hoveredButton_;
    hoveredButton_ = nullptr;
    if (previous && IsWindow(previous)) {
        InvalidateRect(previous, nullptr, FALSE);
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

} // namespace backtrack
