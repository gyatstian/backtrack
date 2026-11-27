#include "studio/StudioWindowImpl.h"
#include "studio/StudioTheme.h"

#include "core/Logger.h"
#include "ui_common/DiagnosticText.h"
#include "ui_common/SettingsPublication.h"
#include "ui_common/WindowTools.h"

#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <string_view>

namespace backtrack {

void StudioWindow::Impl::buildDiagnostics() {
    const auto output = [this](const wchar_t* help) {
        auto edit = make(L"EDIT", L"", WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                         {}, help);
        // The margin is in pixels, so updateFont reapplies it on a DPI change.
        if (edit) {
            controls.at(edit).margin = 6;
            controls.at(edit).editBorder = true;
            SetWindowLongPtrW(edit, GWL_STYLE, GetWindowLongPtrW(edit, GWL_STYLE) | WS_BORDER);
            SetWindowPos(edit, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(6), px(6)));
        return edit;
    };
    section(L"Runtime");
    diagnostics[0] = output(L"Capture runtime, replay and integration statistics.");
    section(L"Encoder");
    diagnostics[1] = output(L"Device capabilities and effective encoder settings.");
    section(L"Recent log");
    auto logging = make(L"BUTTON", L"Enable logging", WS_TABSTOP | WS_VISIBLE | BS_AUTOCHECKBOX,
        [this](HWND hwnd, UINT event) {
            if (event != BN_CLICKED) return;
            const wchar_t* error;
            if (busy || shuttingDown) {
                error = L"Recorder is busy. Try again when it finishes.";
            } else {
                auto candidate = saved;
                candidate.loggingEnabled = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
                error = publishWindowSettings(store, controller, saved, candidate, readable);
            }
            if (error) {
                SendMessageW(hwnd, BM_SETCHECK, saved.loggingEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                setStatus(error);
            } else {
                draft.loggingEnabled = saved.loggingEnabled;
                updateDiagnostics();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }, L"Applies as soon as it changes, unlike the settings pages.");
    if (logging) controls.at(logging).checkbox = true;
    SendMessageW(logging, BM_SETCHECK, saved.loggingEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    const auto open = button(L"Open log folder", [this] {
        const auto folder = Logger::instance().currentPath().parent_path();
        if (folder.empty() || !shellExecuteSucceeded(ShellExecuteW(window, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)))
            setStatus(L"Could not open log folder.");
    });
    const auto saveLogButton = button(L"Save log\u2026", [this] { saveLog(); });
    const auto copyButton = button(L"Copy", [this] { copyDiagnostics(); },
                                   L"Copy runtime, encoder, and recent log text to the clipboard.");
    diagnostics[2] = output(L"Recent lines from the session log.");
    rows.back().cells = {logging, open, saveLogButton, copyButton};
}

void StudioWindow::Impl::layoutDiagnostics() {
    RECT r{};
    GetClientRect(body, &r);
    const int inset = px(studio_theme::inset), gap = px(studio_theme::gap);
    const int top = px(3), textTop = top + px(rows[0].height);
    const int actionHeight = px(28), openWidth = px(122), saveWidth = px(88), copyWidth = px(60);
    const int right = (std::max)(inset, static_cast<int>(r.right) - inset);
    const int available = (std::max)(0, right - inset - gap * 2);
    // Keep Runtime and Encoder visible even at high zoom on a narrow window.
    const int maxLogWidth = available / 2;
    const int buttonsWidth = openWidth + gap + saveWidth + gap + copyWidth;
    const int logWidth = (std::min)(maxLogWidth, (std::max)(available / 3, buttonsWidth));
    const int leftWidth = (available - logWidth) / 2;
    const int middle = inset + leftWidth + gap, log = right - logWidth;
    const int bottom = r.bottom - px(5);
    const int actionsTop = textTop + actionHeight + gap;
    const int logTop = actionsTop + actionHeight + gap;
    int fittedOpen = openWidth, fittedSave = saveWidth, fittedCopy = copyWidth;
    if (buttonsWidth > logWidth) {
        const int usable = (std::max)(0, logWidth - gap * 2);
        const int totalBase = openWidth + saveWidth + copyWidth;
        fittedOpen = MulDiv(usable, openWidth, totalBase);
        fittedSave = MulDiv(usable, saveWidth, totalBase);
        fittedCopy = (std::max)(0, usable - fittedOpen - fittedSave);
    }
    headingRects = {{inset, top, inset + leftWidth, textTop}, {middle, top, log - gap, textTop}, {log, top, right, textTop}};
    const auto& actions = rows.back().cells;
    const std::vector<WindowMove> moves{
        {diagnostics[0], inset, textTop, leftWidth, (std::max)(0, bottom - textTop)},
        {diagnostics[1], middle, textTop, (std::max)(0, log - gap - middle), (std::max)(0, bottom - textTop)},
        {actions[0], log, textTop, logWidth, actionHeight},
        {actions[1], right - fittedCopy - gap - fittedSave - gap - fittedOpen, actionsTop, fittedOpen, actionHeight},
        {actions[2], right - fittedCopy - gap - fittedSave, actionsTop, fittedSave, actionHeight},
        {actions[3], right - fittedCopy, actionsTop, fittedCopy, actionHeight},
        {diagnostics[2], log, logTop, logWidth, (std::max)(0, bottom - logTop)},
    };
    applyWindowMoves(moves);
}

void StudioWindow::Impl::saveLog() {
    if (auto result = exportDiagnostics(window, controller); !result.empty()) setStatus(std::move(result));
}

void StudioWindow::Impl::copyDiagnostics() {
    const std::wstring joined = L"=== Runtime ===\r\n" + readText(diagnostics[0]) +
        L"\r\n\r\n=== Encoder ===\r\n" + readText(diagnostics[1]) +
        L"\r\n\r\n=== Recent log ===\r\n" + readText(diagnostics[2]);
    if (!OpenClipboard(window)) {
        setStatus(L"Could not open the clipboard.");
        return;
    }
    EmptyClipboard();
    const size_t bytes = (joined.size() + 1) * sizeof(wchar_t);
    if (const HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* dst = GlobalLock(mem)) {
            std::memcpy(dst, joined.c_str(), bytes);
            GlobalUnlock(mem);
            if (SetClipboardData(CF_UNICODETEXT, mem)) {
                CloseClipboard();
                setStatus(L"Diagnostics copied to clipboard.");
                return;
            }
        }
        GlobalFree(mem);
    }
    CloseClipboard();
    setStatus(L"Could not copy diagnostics.");
}

void StudioWindow::Impl::updateDiagnostics() {
    if (nav.page != Page::Diagnostics && !controller.isRecording() && audioWarning.empty()) return;
    const auto stats = controller.stats();
    auto warning = audioLivenessWarning(stats);
    if (warning != audioWarning) {
        audioWarning = std::move(warning);
        paintFooter();
    }
    if (nav.page != Page::Diagnostics) return;
    const auto text = diagnosticText(controller, stats, voice.stats(), league.stats());
    // The encoder column only changes when an action applies settings or the
    // backend is re-initialized, both of which bump the action revision; building
    // it on every poll would spend the tick on unchanged text.
    if (encoderText.empty() || encoderTextRevision != actionRevision) {
        encoderText = encoderDiagnosticText(controller);
        encoderTextRevision = actionRevision;
    }
    const std::array<const std::wstring*, 3> columns{&text[0], &encoderText, &text[1]};
    const auto focusedField = GetFocus();
    std::wstring current;
    for (size_t i = 0; i < columns.size(); ++i) {
        const auto field = diagnostics[i];
        if (!field || field == focusedField) continue;
        const auto& next = *columns[i];
        const auto length = SendMessageW(field, WM_GETTEXTLENGTH, 0, 0);
        if (static_cast<size_t>(length) == next.size()) {
            current.resize(next.size() + 1);
            const auto copied = SendMessageW(field, WM_GETTEXT, current.size(), reinterpret_cast<LPARAM>(current.data()));
            if (copied == length && std::wstring_view(current.data(), next.size()) == next) continue;
        }
        const auto first = SendMessageW(field, EM_GETFIRSTVISIBLELINE, 0, 0);
        if (SetWindowTextW(field, next.c_str()) && first) SendMessageW(field, EM_LINESCROLL, 0, first);
    }
}

} // namespace backtrack
