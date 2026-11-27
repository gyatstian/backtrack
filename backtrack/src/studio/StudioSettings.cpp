#include "studio/StudioWindowImpl.h"
#include "studio/StudioTheme.h"

#include "audio/WasapiCapture.h"
#include "platform/Win32Util.h"
#include "ui_common/SettingsPublication.h"
#include "ui_common/WindowTools.h"

#include <shellapi.h>

#include <algorithm>
#include <cassert>
#include <memory>

namespace backtrack {
namespace {
constexpr auto settingsHelp = L"Save changes before they affect capture.";
constexpr auto secondsHelp = L"Longer replay buffers retain more footage and take more disk or memory space.";
constexpr auto hotkeyHelp = L"Focus this field and press a shortcut. Press Delete or Backspace to clear it.";
constexpr auto columnsHelp = L"Gallery columns. Blank or 0 fits as many as the window allows; a larger count leaves the tiles smaller.";

} // namespace

HWND StudioWindow::Impl::editField(const std::wstring& value, DWORD style, ControlEvent onChange, const wchar_t* help) {
    const auto field = make(L"EDIT", value.c_str(), WS_TABSTOP | WS_VISIBLE | WS_BORDER | style, std::move(onChange), help);
    // The margin is in pixels, so updateFont reapplies it on a DPI change.
    if (field) {
        auto& control = controls.at(field);
        control.margin = 5;
        control.editBorder = true;
    }
    SendMessageW(field, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(5), px(5)));
    return field;
}

HWND StudioWindow::Impl::edit(const wchar_t* label, const std::wstring& value, DWORD style,
                              ControlEvent onChange, const wchar_t* help) {
    auto title = make(L"STATIC", label, SS_CENTERIMAGE | SS_NOTIFY | SS_NOPREFIX | SS_ENDELLIPSIS | WS_VISIBLE, {}, help);
    const auto field = editField(value, style, std::move(onChange), help);
    row({title, field}, true);
    return field;
}

HWND StudioWindow::Impl::checkbox(const wchar_t* title, bool& value, const wchar_t* help) {
    const auto control = make(L"BUTTON", title, WS_TABSTOP | WS_VISIBLE | BS_AUTOCHECKBOX,
        [this, &value](HWND hwnd, UINT event) {
            if (event != BN_CLICKED) return;
            value = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
            InvalidateRect(hwnd, nullptr, FALSE);
            changed();
        }, help);
    if (control) controls.at(control).checkbox = true;
    SendMessageW(control, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    return control;
}

void StudioWindow::Impl::check(const wchar_t* title, bool& value, const wchar_t* help) {
    row({checkbox(title, value, help)});
}

HWND StudioWindow::Impl::numberField(uint32_t& value, const uint32_t& fallback, const wchar_t* help) {
    const auto field = editField(std::to_wstring(value), ES_NUMBER | ES_AUTOHSCROLL,
        [this, &value, &fallback](HWND hwnd, UINT event) {
            if (event != EN_CHANGE) return;
            if (invalidSaveAttempt == hwnd) invalidSaveAttempt = nullptr;
            const auto parsed = parseUIntControl(hwnd);
            if (!parsed) {
                const bool reset = std::exchange(value, fallback) != fallback;
                const bool inserted = invalidNumbers.insert(hwnd).second;
                if (inserted) RedrawWindow(hwnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
                if (inserted || reset) changed();
                return;
            }
            const bool wasInvalid = invalidNumbers.erase(hwnd) != 0;
            if (wasInvalid) RedrawWindow(hwnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
            if (const auto it = controls.find(hwnd); it != controls.end() && it->second.sliderBuddy)
                InvalidateRect(it->second.sliderBuddy, nullptr, FALSE);
            if (std::exchange(value, *parsed) != *parsed || wasInvalid) {
                if (&value == &draft.audioOutputVolumePercent || &value == &draft.audioInputVolumePercent)
                    controller.previewAudioVolumes(draft.audioOutputVolumePercent, draft.audioInputVolumePercent);
                changed();
            }
        }, help);
    if (field) controls.at(field).number = true;
    return field;
}

HWND StudioWindow::Impl::gainSlider(HWND field, uint32_t maxValue, const wchar_t* help) {
    const auto slider = make(L"STATIC", L"", WS_VISIBLE | WS_TABSTOP | SS_NOTIFY, {}, help);
    if (slider && field) {
        auto& s = controls.at(slider);
        s.slider = true;
        s.sliderMax = maxValue;
        s.sliderBuddy = field;
        controls.at(field).sliderBuddy = slider;
    }
    return slider;
}

void StudioWindow::Impl::number(const wchar_t* title, uint32_t& value, const uint32_t& fallback, const wchar_t* help) {
    auto label = make(L"STATIC", title, SS_CENTERIMAGE | SS_NOTIFY | SS_NOPREFIX | SS_ENDELLIPSIS | WS_VISIBLE, {}, help);
    row({label, numberField(value, fallback, help)}, true);
    rows.back().widths = {0, 88};
}

void StudioWindow::Impl::path(const wchar_t* title, std::filesystem::path& value, bool folder, const wchar_t* help) {
    const auto field = edit(title, value.wstring(), ES_AUTOHSCROLL,
        [this, &value](HWND hwnd, UINT event) {
            if (event != EN_CHANGE) return;
            auto text = trimText(readText(hwnd));
            if (value.native() == text) return;
            value = std::move(text);
            changed();
        }, help);
    rows.back().cells.push_back(button(L"Browse\u2026", [this, field, folder, title] {
        if (folder) {
            std::wstring selected;
            if (pickFolder(window, title, selected)) setText(field, selected);
        } else {
            const COMDLG_FILTERSPEC wav{L"WAV audio", L"*.wav"};
            std::filesystem::path selected;
            if (pickFileForOpen(window, title, &wav, 1, L"wav", selected)) setText(field, selected.wstring());
        }
    }, help));
    rows.back().tail = true;
}

HWND StudioWindow::Impl::choices(const wchar_t* title, const Choices& values, int selected, const wchar_t* help) {
    const auto label = make(L"STATIC", title, SS_CENTERIMAGE | SS_NOTIFY | SS_NOPREFIX | SS_ENDELLIPSIS | WS_VISIBLE, {}, help);
    const auto combo = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST |
        CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, {}, help);
    if (combo) {
        controls.at(combo).combo = true;
        // WM_COMMAND copies the handler so it can outlive the control during dispatch.
        auto actions = std::make_shared<std::vector<Action>>();
        actions->reserve(values.size());
        for (const auto& [text, chosen] : values) {
            const auto index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            if (index < 0) break;
            assert(static_cast<size_t>(index) == actions->size());
            actions->push_back(chosen);
        }
        controls.at(combo).event = [this, actions = std::move(actions)](HWND hwnd, UINT event) {
            if (event != CBN_SELCHANGE) return;
            const auto i = SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
            if (i >= 0 && static_cast<size_t>(i) < actions->size()) {
                (*actions)[i]();
                changed();
            }
        };
        SendMessageW(combo, CB_SETITEMHEIGHT, -1, px(21));
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
    }
    row({label, combo}, true);
    return combo;
}

HWND StudioWindow::Impl::hotkeyBox(uint32_t& modifiers, uint32_t& key) {
    const auto field = editField(hotkeyDisplay(modifiers, key), ES_READONLY | ES_AUTOHSCROLL, {}, hotkeyHelp);
    if (field) hotkeyFields.emplace(field, std::pair{&modifiers, &key});
    return field;
}

void StudioWindow::Impl::device(AudioTrack track, std::wstring& id) {
    constexpr auto help = L"Default follows Windows. A selected device stays pinned even when disconnected.";
    const bool output = track == AudioTrack::System;
    const size_t index = output ? 0 : 1;
    const auto label = make(L"STATIC", output ? L"Output device" : L"Input device",
                            SS_CENTERIMAGE | SS_NOTIFY | SS_NOPREFIX | SS_ENDELLIPSIS | WS_VISIBLE, {}, help);
    const auto combo = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST |
        CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, [this, index, &id](HWND hwnd, UINT event) {
            const auto& device = deviceCombos[index];
            if (event == CBN_SELCHANGE) {
                const auto selected = SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
                if (selected >= 0 && static_cast<size_t>(selected) < device.ids.size()) {
                    id = device.ids[selected];
                    changed();
                } else {
                    // Revert if the user clicked the "Loading devices..." placeholder row.
                    const auto current = std::ranges::find(device.ids, id);
                    SendMessageW(hwnd, CB_SETCURSEL,
                                 current != device.ids.end() ? static_cast<WPARAM>(current - device.ids.begin()) : 0, 0);
                }
            } else if (event == CBN_CLOSEUP && device.refresh) {
                // The final CBN_SELCHANGE can follow CBN_CLOSEUP.
                PostMessageW(window, devicesReady, 0, 0);
            }
        }, help);
    row({label, combo}, true);
    if (!combo) return;
    controls.at(combo).combo = true;
    deviceCombos[index].hwnd = combo;
    SendMessageW(combo, CB_SETITEMHEIGHT, -1, px(21));
    fillDeviceCombo(index, id);
}

void StudioWindow::Impl::fillDeviceCombo(size_t index, const std::wstring& id) {
    auto& device = deviceCombos[index];
    const bool loading = deviceWorker.joinable();
    static const std::vector<AudioDeviceInfo> noEndpoints;
    auto list = deviceChoices(loading ? noEndpoints : deviceEndpoints[index], id);
    device.ids.reserve(list.size() + 1);
    const auto pinned = id.empty() ? list.end() : std::ranges::find(list, id, &AudioDeviceInfo::id);
    SendMessageW(device.hwnd, CB_RESETCONTENT, 0, 0);
    device.ids.clear();
    LRESULT selected = CB_ERR;
    const auto add = [&](const wchar_t* label, std::wstring&& endpointId) {
        const auto item = SendMessageW(device.hwnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        if (item < 0) return;
        assert(static_cast<size_t>(item) == device.ids.size());
        if (endpointId == id) selected = item;
        device.ids.push_back(std::move(endpointId));
    };
    add(L"Default", {});
    if (pinned != list.end()) add(pinned->name.c_str(), std::move(pinned->id));
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it != pinned) add(it->name.c_str(), std::move(it->id));
    }
    if (loading) SendMessageW(device.hwnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Loading devices\u2026"));
    SendMessageW(device.hwnd, CB_SETCURSEL, selected, 0);
    InvalidateRect(device.hwnd, nullptr, FALSE);
    device.refresh = false;
}

void StudioWindow::Impl::completeDeviceDiscovery() {
    if (building || closing || shuttingDown) return;
    if (deviceWorker.joinable()) {
        if (!deviceWorkerDone.load()) return;
        deviceWorker.join();
        for (auto& device : deviceCombos) device.refresh = device.hwnd != nullptr;
    }
    for (size_t i = 0; i < deviceCombos.size(); ++i) {
        const auto& device = deviceCombos[i];
        if (device.refresh && body && GetParent(device.hwnd) == body && controls.contains(device.hwnd) &&
            !SendMessageW(device.hwnd, CB_GETDROPPEDSTATE, 0, 0)) {
            fillDeviceCombo(i, i == 0 ? draft.audioOutputDeviceId : draft.audioInputDeviceId);
        }
    }
}

HWND StudioWindow::Impl::lastField() const {
    assert(!rows.empty() && !rows.back().cells.empty());
    return rows.back().cells[rows.back().split ? 1 : 0];
}

void StudioWindow::Impl::atControlColumn() {
    assert(!rows.empty());
    rows.back().controlColumn = true;
}

void StudioWindow::Impl::enabledWhen(std::function<bool()> predicate, int depth) {
    rows.back().dependencyDepth = depth;
    conditions.emplace_back(rows.size() - 1, std::move(predicate));
}

void StudioWindow::Impl::visibleWhen(HWND control, std::function<bool()> predicate) {
    visibility.emplace_back(control, std::move(predicate));
}

HWND StudioWindow::Impl::invalidNumber() const {
    for (const auto field : invalidNumbers) {
        if (IsWindowVisible(field)) return field;
    }
    return nullptr;
}

bool StudioWindow::Impl::dirty() const {
    return draft != saved || invalidNumber();
}

void StudioWindow::Impl::changed() {
    if (building) return;
    bool reflow = false;
    for (const auto& [index, available] : conditions) {
        auto& item = rows[index];
        const bool show = available();
        if (item.hidden == !show) continue;
        item.hidden = !show;
        for (const auto hwnd : item.cells) if (hwnd) ShowWindow(hwnd, show ? SW_SHOWNA : SW_HIDE);
        reflow = true;
    }
    for (const auto& [control, available] : visibility)
        ShowWindow(control, available() ? SW_SHOWNA : SW_HIDE);
    if (reflow) layoutRows();
    syncActionButtons();
}

void StudioWindow::Impl::syncActionButtons() {
    const bool idle = !busy && !closing && !shuttingDown;
    const auto setButtonState = [this](HWND btn, bool enabled, const wchar_t* label) {
        if (!btn) return;
        if (label) setText(btn, label);
        if ((IsWindowEnabled(btn) != FALSE) != enabled) {
            if (!enabled && GetFocus() == btn) SetFocus(pageButtons[static_cast<size_t>(nav.page)]);
            EnableWindow(btn, enabled);
        }
        InvalidateRect(btn, nullptr, FALSE);
    };
    if (record) {
        const bool recording = motions[recordGlyph].to != 0.f;
        const wchar_t* label = recording ? L"Stop recording" : L"Start recording";
        if (busy && activeAction == ControllerActionKind::Startup) label = L"Starting\u2026";
        else if (busy && activeAction == ControllerActionKind::ToggleRecording)
            label = recording ? L"Stopping\u2026" : L"Starting\u2026";
        setButtonState(record, idle, label);
    }
    if (replayButton) {
        const wchar_t* label = busy && activeAction == ControllerActionKind::SaveReplay ? L"Saving\u2026" : L"Save";
        setButtonState(replayButton, idle, label);
    }
    const bool isDirty = dirty();
    if (save) {
        const wchar_t* label = busy && activeAction == ControllerActionKind::ApplySettings ? L"Applying\u2026" : L"Save settings";
        if (const auto it = controls.find(save); it != controls.end())
            it->second.tone = isDirty ? ButtonTone::Primary : ButtonTone::Normal;
        setButtonState(save, isDirty && idle, label);
    }
    if (discard) {
        const bool wasVisible = (GetWindowLongPtrW(discard, GWL_STYLE) & WS_VISIBLE) != 0;
        if (wasVisible != isDirty) {
            if (!isDirty && GetFocus() == discard) SetFocus(pageButtons[static_cast<size_t>(nav.page)]);
            ShowWindow(discard, isDirty ? SW_SHOWNA : SW_HIDE);
            RECT client{};
            if (GetClientRect(window, &client)) {
                layoutFooter(client);
                client.top = client.bottom - px(studio_theme::footerHeight);
                InvalidateRect(window, &client, FALSE);
            }
        }
        setButtonState(discard, isDirty && idle, nullptr);
    }
}

void StudioWindow::Impl::discardSettings() {
    if (!dirty() || busy || shuttingDown) return;
    draft = saved;
    controller.previewAudioVolumes(saved.audioOutputVolumePercent, saved.audioInputVolumePercent);
    const int previous = scroll;
    buildPage();
    scrollTo(previous);
    setStatus(L"Changes discarded.");
}

bool StudioWindow::Impl::saveSettings(bool refreshPage) {
    if (const auto invalid = invalidNumber()) {
        invalidSaveAttempt = invalid;
        SetFocus(invalid);
        RedrawWindow(invalid, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
        setStatus(L"Enter a whole number.");
        return false;
    }
    if (busy || shuttingDown) { setStatus(L"Recorder is busy. Try saving again when it finishes."); return false; }
    if (!readable) { setStatus(L"Settings could not be read at startup. Restart after the file becomes readable."); return false; }
    const auto candidate = sanitizeSettings(draft);
    // sanitizeSettings clamps on save, so a difference here means a value the
    // user typed was adjusted rather than stored as written.
    const bool adjusted = candidate != draft;
    if (candidate != saved) {
        const auto ensure = [this](const std::filesystem::path& directory, const wchar_t* label) {
            if (directory.empty()) return true;
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (!error) return true;
            setStatus(std::wstring(label) + L" could not be created: " + utf8ToWide(error.message()));
            return false;
        };
        if (!ensure(candidate.clipDirectory, L"Clip folder")) return false;
        if ((candidate.replay.diskBackedVideo || candidate.replay.diskBackedAudio) &&
            candidate.replayRingDirectory != candidate.clipDirectory &&
            !ensure(candidate.replayRingDirectory, L"Replay folder")) return false;
        bool published = false;
        const auto warning = publishWindowSettings(store, controller, saved, candidate, readable, &published);
        if (!published) {
            setStatus(warning);
            return false;
        }
        // The zoom mode applies to the live window, which no page rebuild covers
        // when it is the only edit.
        layout();
        ControllerAction settings;
        settings.kind = ControllerActionKind::ApplySettings;
        settings.settings = saved;
        settings.hotkeysOk = hotkeys.registerHotkeys(window, registeredHotkeys(saved));
        settings.hotkeyError = hotkeys.lastErrorMessage();
        // The startup-entry warning outranks the clamp notice: it names
        // something the user has to act on.
        std::wstring adjustedMessage;
        if (adjusted && !warning) {
            std::wstring fields;
            const auto note = [&fields](const wchar_t* name) {
                if (!fields.empty()) fields += L", ";
                fields += name;
            };
            if (draft.video.resolutionMode == ResolutionMode::Custom) {
                if (draft.video.width != candidate.video.width) note(L"Width");
                if (draft.video.height != candidate.video.height) note(L"Height");
            }
            if (draft.video.fps != candidate.video.fps) note(L"Frame rate");
            if (draft.video.bitrateKbps != candidate.video.bitrateKbps) note(L"Bitrate");
            if (draft.video.gopSeconds != candidate.video.gopSeconds) note(L"Keyframe interval");
            if (draft.video.encoderLookaheadDepth != candidate.video.encoderLookaheadDepth) note(L"Lookahead depth");
            if (draft.video.encoderAQStrength != candidate.video.encoderAQStrength) note(L"Spatial AQ strength");
            if (draft.video.encoderReferenceFrames != candidate.video.encoderReferenceFrames) note(L"Reference frames");
            if (draft.replay.seconds != candidate.replay.seconds) note(L"Replay length");
            if (draft.replay.overlapSeconds != candidate.replay.overlapSeconds) note(L"Clip overlap");
            if (draft.gpu.frameQueueLimit != candidate.gpu.frameQueueLimit) note(L"Frame queue limit");
            if (draft.notificationSoundVolumePercent != candidate.notificationSoundVolumePercent) note(L"Volume");
            if (draft.audioOutputVolumePercent != candidate.audioOutputVolumePercent) note(L"Desktop gain");
            if (draft.audioInputVolumePercent != candidate.audioInputVolumePercent) note(L"Microphone gain");
            if (draft.clipDirectory != candidate.clipDirectory) note(L"Clip folder");
            if (draft.replayRingDirectory != candidate.replayRingDirectory) note(L"Buffer folder");
            if (draft.hotkeys != candidate.hotkeys) note(L"Hotkeys");
            adjustedMessage = fields.empty()
                ? L"Saved; some values were adjusted to their allowed range"
                : L"Saved; adjusted to allowed range: " + fields;
        }
        const std::wstring applied = warning ? warning
            : adjusted ? adjustedMessage
                       : L"Settings saved. Applying capture changes\u2026";
        submit(std::move(settings), applied.c_str());
    }
    const bool normalized = draft != saved;
    if (normalized) draft = saved;
    if (refreshPage) {
        const HWND prevFocus = GetFocus();
        const int prevScroll = scroll;
        std::optional<std::pair<size_t, size_t>> bodyFocus;
        for (size_t r = 0; r < rows.size() && !bodyFocus; ++r) {
            for (size_t c = 0; c < rows[r].cells.size(); ++c) {
                if (rows[r].cells[c] == prevFocus) {
                    bodyFocus = {r, c};
                    break;
                }
            }
        }
        if (normalized || !invalidNumbers.empty()) {
            buildPage();
            scrollTo(prevScroll);
            HWND restored = nullptr;
            if (bodyFocus && bodyFocus->first < rows.size() && bodyFocus->second < rows[bodyFocus->first].cells.size()) {
                const HWND candidate = rows[bodyFocus->first].cells[bodyFocus->second];
                if (candidate && IsWindowVisible(candidate) && IsWindowEnabled(candidate)) {
                    RECT view{}, r{};
                    GetClientRect(body, &view);
                    GetWindowRect(candidate, &r);
                    MapWindowPoints(nullptr, body, reinterpret_cast<POINT*>(&r), 2);
                    if (r.top >= 0 && r.bottom <= view.bottom) restored = candidate;
                }
            }
            if (restored) SetFocus(restored);
            else SetFocus(nav.page == Page::Settings ? categoryButtons[static_cast<size_t>(nav.category)] : pageButtons[0]);
        } else {
            changed();
            if (!prevFocus || prevFocus == save || prevFocus == discard || !IsWindowVisible(prevFocus) || !IsWindowEnabled(prevFocus))
                SetFocus(nav.page == Page::Settings ? categoryButtons[static_cast<size_t>(nav.category)] : pageButtons[0]);
        }
    }
    return true;
}

bool StudioWindow::Impl::leaveSettings(bool refreshPage) {
    if (!dirty()) return true;
    const int answer = MessageBoxW(window, L"Save your changes before leaving?", L"Unsaved settings", MB_ICONWARNING | MB_YESNOCANCEL);
    if (answer == IDYES) return saveSettings(refreshPage);
    if (answer != IDNO) return false;
    draft = saved;
    controller.previewAudioVolumes(saved.audioOutputVolumePercent, saved.audioInputVolumePercent);
    if (refreshPage) buildPage();
    return true;
}

void StudioWindow::Impl::addUpdateAction() {
    update = button(L"Update available", [this] {
        if (!shellExecuteSucceeded(ShellExecuteW(window, L"open", L"https://github.com/gyatstian/backtrack/releases",
                                                 nullptr, nullptr, SW_SHOWNORMAL))) setStatus(L"Could not open the releases page.");
    }, L"", window, ButtonTone::Quiet);
}

void StudioWindow::Impl::buildCapture() {
    constexpr int actionWidth = 160;

    section(L"Recording");
    auto& keys = draft.hotkeys;
    // Rebuilding the page must not expose an action that is still finalizing.
    record = button(motions[recordGlyph].to != 0.f ? L"Stop recording" : L"Start recording", [this] {
        submit(ControllerActionKind::ToggleRecording,
               controller.isRecording() ? L"Stopping recording\u2026" : L"Starting recording\u2026", clickOrigin == record);
    }, L"", nullptr, ButtonTone::Primary);
    if (record) controls.at(record).pointerMotion = true;
    row({record, hotkeyBox(keys.startStopModifiers, keys.startStopVirtualKey)});
    rows.back().widths = {actionWidth, 164};

    recover = button(L"Recover failed recording", [this] {
        submit(ControllerActionKind::RecoverFailedRecording, L"Recovering recording\u2026");
    }, L"", nullptr, ButtonTone::Quiet);
    recoveryRow = rows.size();
    row({recover}, false, 28);
    rows.back().widths = {176};
    syncRecoveryAction();

    section(L"Instant replay");
    row({checkbox(L"Enable replay", draft.replay.enabled, L""),
         hotkeyBox(keys.saveReplayModifiers, keys.saveReplayVirtualKey)});
    rows.back().widths = {136, 164};
    visibleWhen(rows.back().cells[1], [this] { return draft.replay.enabled; });
    replayButton = button(L"Save", [this] { requestReplay(clickOrigin == replayButton); },
                          L"", nullptr, ButtonTone::Primary);
    if (replayButton) controls.at(replayButton).pointerMotion = true;
    row({replayButton, make(L"STATIC", L"the last", SS_CENTERIMAGE | WS_VISIBLE),
         numberField(draft.replay.seconds, saved.replay.seconds, secondsHelp),
         make(L"STATIC", L"seconds", SS_CENTERIMAGE | WS_VISIBLE)});
    rows.back().widths = {80, 54, 60, 64};
    enabledWhen([this] { return draft.replay.enabled; }, 0);

    save = button(L"Save settings", [this] { saveSettings(); }, settingsHelp, window);
    discard = button(L"Discard", [this] { discardSettings(); }, L"Revert unsaved settings changes.", window, ButtonTone::Quiet);
    if (discard) ShowWindow(discard, SW_HIDE);
}

void StudioWindow::Impl::syncRecoveryAction() {
    if (!recover) return;
    const bool show = recoveryAvailable && !busy && !controller.isRecording();
    auto& height = rows[recoveryRow].height;
    const int nextHeight = show ? 28 : 0;
    if (height == nextHeight) return;
    height = nextHeight;
    if (!show && GetFocus() == recover) SetFocus(record);
    ShowWindow(recover, show ? SW_SHOWNA : SW_HIDE);
    if (!building) layoutRows();
}

void StudioWindow::Impl::buildSettings() {
    const auto enumChoice = [this](const wchar_t* title, const auto& table, auto& target, const wchar_t* help = L"") {
        using T = std::remove_reference_t<decltype(target)>;
        Choices values;
        int selected = 0;
        for (const auto& [value, label] : table) {
            if (static_cast<int>(target) == value) selected = static_cast<int>(values.size());
            values.emplace_back(label, [&target, value] { target = static_cast<T>(value); });
        }
        choices(title, values, selected, help);
    };
    auto& video = draft.video;
    auto& game = draft.gameIntegrations;
    switch (nav.category) {
    case Category::App: {
        section(L"Application");
        check(L"Start minimized with Windows", draft.startWithWindowsMinimized,
              L"Adds Backtrack to Windows startup. It starts minimized only when Windows launches it.");
        check(L"Close to the tray", draft.exitToTray, L"Closing the window leaves Backtrack running. Use Exit on the tray icon to quit.");
        check(L"Check for updates at startup", draft.checkForUpdates, L"Asks GitHub for a new release once per launch.");
        check(L"Scale UI with app", draft.uiScaleWithWindow,
              L"Fits the UI to the window size. Ctrl+Scroll scales it either way.");

        section(L"Library");
        const auto columns = make(L"STATIC", L"Columns", SS_CENTERIMAGE | SS_NOTIFY | SS_NOPREFIX | SS_ENDELLIPSIS | WS_VISIBLE, {},
                                  columnsHelp);
        // A raw count, not numberField: blank means auto, so an empty field is a
        // value rather than an invalid edit.
        const auto field = editField(draft.libraryColumns ? std::to_wstring(draft.libraryColumns) : std::wstring(),
            ES_NUMBER | ES_AUTOHSCROLL,
            [this](HWND hwnd, UINT event) {
                if (event != EN_CHANGE) return;
                const uint32_t value = readText(hwnd).empty() ? 0
                    : parseUIntControl(hwnd).value_or(draft.libraryColumns);
                if (std::exchange(draft.libraryColumns, value) != value) changed();
            }, columnsHelp);
        row({columns, field}, true);
        rows.back().widths = {0, 88};
        check(L"Fit thumbnails", draft.libraryFitThumbnails,
              L"Gives the tile the frame's shape, so the thumbnail fills it. Off keeps one tile height and shows bars on wide tiles.");
        check(L"Separate library scale", draft.libraryOwnScale,
              L"Gives the gallery its own Ctrl+Scroll scale instead of the app's.");
        check(L"Show hidden folders", draft.libraryShowHiddenFolders,
              L"Shows folders whose names start with a dot.");
        break;
    }
    case Category::Capture: {
        section(L"Capture");
        path(L"Clip folder", draft.clipDirectory, true);
        enumChoice(L"Capture method", kCaptureBackendComboEntries, draft.preferredCaptureBackend,
                   L"WGC is the default. Duplication is tried first on HDR displays and whenever WGC fails.");
        const auto storage = make(L"STATIC", L"Store on disk", SS_CENTERIMAGE | SS_NOTIFY | SS_ENDELLIPSIS | WS_VISIBLE, {},
                                  L"Keeps the buffered replay on the drive instead of in memory.");
        row({storage, checkbox(L"Video", draft.replay.diskBackedVideo, L""),
                      checkbox(L"Audio", draft.replay.diskBackedAudio, L"")}, true);
        rows.back().widths = {0, 76, 76};
        path(L"Buffer folder", draft.replayRingDirectory, true, L"Leave blank to keep the buffer files in the clip folder.");
        enabledWhen([this] { return draft.replay.diskBackedVideo || draft.replay.diskBackedAudio; });
        number(L"Clip overlap", draft.replay.overlapSeconds, saved.replay.overlapSeconds,
               L"When saving again within the replay window, save only new footage plus this many seconds of the previous clip. Allowed range: 0\u20133600 seconds.");
        rows.back().cells.push_back(make(L"STATIC", L"seconds", SS_CENTERIMAGE | WS_VISIBLE));
        rows.back().widths = {0, 60, 64};
        check(L"Organize clips by game", draft.organizeClipsByGame,
              L"Saves replays in a folder named after the game. Manual recordings stay in the clip folder.");
        check(L"Capture mouse cursor", draft.captureCursor);

        section(L"Multi-monitor");
        check(L"Follow across monitors", draft.multiMonitorSupport,
              L"Keeps capture on the monitor holding the focused app or the pointer.");
        const auto multiMonitor = [this] { return draft.multiMonitorSupport; };
        choices(L"Follow", {{L"Focused monitor", [this] { draft.followMouseMonitor = false; }},
                            {L"Mouse pointer", [this] { draft.followMouseMonitor = true; }}}, draft.followMouseMonitor ? 1 : 0);
        enabledWhen(multiMonitor);
        check(L"Stabilize frames across monitors", draft.gpu.stableMultimonitorFrames,
              L"Copies every frame so a monitor switch cannot resize a texture the encoder still reads.");
        enabledWhen(multiMonitor);
        break;
    }
    case Category::Video: {
        section(L"Quality");
        Choices sizes;
        int selected = 0;
        for (const auto& [value, label] : kResolutionPresetComboEntries) {
            if (static_cast<int>(video.resolutionMode) == value) selected = static_cast<int>(sizes.size());
            sizes.emplace_back(label, [&video, value] { video.resolutionMode = static_cast<ResolutionMode>(value); });
        }
        for (const auto& monitor : enumerateMonitors()) {
            sizes.emplace_back(L"Display " + std::to_wstring(monitor.index + 1) + L" \u00b7 " +
                std::to_wstring(monitor.width) + L" \u00d7 " + std::to_wstring(monitor.height), [this, &video, monitor] {
                    video.resolutionMode = ResolutionMode::Custom;
                    video.width = monitor.width;
                    video.height = monitor.height;
                    setText(resolutionFields[0], std::to_wstring(video.width));
                    setText(resolutionFields[1], std::to_wstring(video.height));
                });
        }
        choices(L"Resolution", sizes, selected, L"A display shortcut uses its native size without changing the capture target.");
        number(L"Width", video.width, saved.video.width, L"Allowed range: 16\u20137680 px (even)."); resolutionFields[0] = lastField();
        enabledWhen([&video] { return video.resolutionMode == ResolutionMode::Custom; });
        number(L"Height", video.height, saved.video.height, L"Allowed range: 16\u20134320 px (even)."); resolutionFields[1] = lastField();
        enabledWhen([&video] { return video.resolutionMode == ResolutionMode::Custom; });
        number(L"Frame rate (fps)", video.fps, saved.video.fps, L"Allowed range: 1\u2013240 fps.");
        number(L"Bitrate (kbps)", video.bitrateKbps, saved.video.bitrateKbps, L"Allowed range: 1000\u2013200000 kbps.");
        section(L"GPU");
        enumChoice(L"Adaptive mode", kGpuAdaptiveComboEntries, draft.gpu.adaptiveMode,
                   L"Drops frames when the encoder falls behind. Aggressive drops earlier than Conservative.");
        number(L"Frame queue limit", draft.gpu.frameQueueLimit, saved.gpu.frameQueueLimit,
               L"1\u20138 frames. Captured frames allowed to wait for the encoder. A deeper queue smooths short stalls and adds latency.");
        check(L"Coalesce idle frames", draft.gpu.allowIdleFrameSkipping, L"Skips encoding a frame when the screen did not change.");
        section(L"Format");
        enumChoice(L"Codec", kVideoCodecComboEntries, video.codec);
        enumChoice(L"Profile", kEncoderProfileComboEntries, video.encoderProfile,
                   L"Lowest GPU and Balanced pick a fixed encoder setup. Custom exposes the settings below.");
        section(L"Encoder");
        const auto custom = [&video] { return video.encoderProfile == EncoderProfile::Custom; };
        enumChoice(L"Preset", kEncoderPresetComboEntries, video.encoderPreset); enabledWhen(custom);
        enumChoice(L"Mode", kEncoderModeComboEntries, video.encoderMode); enabledWhen(custom);
        enumChoice(L"Multipass", kEncoderMultipassComboEntries, video.encoderMultipass,
                   L"Encodes each frame twice for better quality at the same bitrate, at extra GPU cost."); enabledWhen(custom);
        check(L"Lookahead", video.encoderLookahead, L"Lets the encoder weigh later frames before encoding. Adds latency."); enabledWhen(custom);
        number(L"Lookahead depth", video.encoderLookaheadDepth, saved.video.encoderLookaheadDepth,
               L"Allowed range: 0\u201331 frames.");
        enabledWhen([&video, custom] { return custom() && video.encoderLookahead; }, 2);
        check(L"Adaptive I-frames", video.encoderAdaptiveIFrames); enabledWhen([&video, custom] { return custom() && video.encoderLookahead; }, 2);
        check(L"Spatial adaptive quantization", video.encoderSpatialAQ); enabledWhen(custom);
        number(L"Spatial AQ strength", video.encoderAQStrength, saved.video.encoderAQStrength,
               L"Allowed range: 1\u201315.");
        enabledWhen([&video, custom] { return custom() && video.encoderSpatialAQ; }, 2);
        check(L"Temporal adaptive quantization", video.encoderTemporalAQ); enabledWhen(custom);
        check(L"B-frames", video.encoderBFrames); enabledWhen(custom);
        check(L"Adaptive B-frames", video.encoderAdaptiveBFrames);
        enabledWhen([&video, custom] { return custom() && video.encoderBFrames && video.encoderLookahead; }, 2);
        number(L"Keyframe interval (seconds)", video.gopSeconds, saved.video.gopSeconds,
               L"1\u201330 seconds. Maximum seconds between keyframes. A replay save starts at a keyframe, so a shorter interval trims more precisely.");
        number(L"Reference frames", video.encoderReferenceFrames, saved.video.encoderReferenceFrames,
               L"Allowed range: 1\u20134."); enabledWhen(custom);
        check(L"Zero reorder delay", video.encoderZeroReorderDelay,
              L"Encodes frames in capture order. Turning it off adds latency and stops idle frames from being coalesced."); enabledWhen(custom);
        break;
    }
    case Category::Audio:
        section(L"Notifications");
        number(L"Volume (%)", draft.notificationSoundVolumePercent, saved.notificationSoundVolumePercent,
               L"Allowed range: 0\u2013100%.");
        rows.back().cells.push_back(gainSlider(lastField(), 100, L"Allowed range: 0\u2013100%."));
        rows.back().widths = {0, 72, 140};
        path(L"Replay success sound", draft.customNotificationSoundPath, false, L"Choose a WAV file or leave blank for the built-in sound.");
        rows.back().cells.push_back(button(L"Test", [this] {
            const uint32_t volume = (std::min)(draft.notificationSoundVolumePercent, uint32_t{100});
            if (volume == 0) {
                setStatus(L"Notification volume is 0%.");
                return;
            }
            if (!draft.customNotificationSoundPath.empty()) {
                std::error_code error;
                if (!std::filesystem::is_regular_file(draft.customNotificationSoundPath, error)) {
                    setStatus(L"Sound file not found.");
                    return;
                }
            }
            playActionIndicator(MB_OK, volume, draft.customNotificationSoundPath);
        }, L"Play the replay success sound at the current volume."));
        rows.back().widths = {0, 0, 88, 64};
        section(L"Devices");
        check(L"Capture system audio", draft.captureSystemAudio);
        device(AudioTrack::System, draft.audioOutputDeviceId); enabledWhen([this] { return draft.captureSystemAudio; });
        number(L"Desktop gain (%)", draft.audioOutputVolumePercent, saved.audioOutputVolumePercent,
               L"Allowed range: 0\u2013200%.");
        rows.back().cells.push_back(gainSlider(lastField(), 200, L"Allowed range: 0\u2013200%."));
        rows.back().widths = {0, 72, 140};
        enabledWhen([this] { return draft.captureSystemAudio; });
        check(L"Capture microphone", draft.captureMicrophone);
        device(AudioTrack::Microphone, draft.audioInputDeviceId); enabledWhen([this] { return draft.captureMicrophone; });
        number(L"Microphone gain (%)", draft.audioInputVolumePercent, saved.audioInputVolumePercent,
               L"Allowed range: 0\u2013200%.");
        rows.back().cells.push_back(gainSlider(lastField(), 200, L"Allowed range: 0\u2013200%."));
        rows.back().widths = {0, 72, 140};
        enabledWhen([this] { return draft.captureMicrophone; });
        check(L"Automatic gain", draft.audioInputAutoGainEnabled,
              L"Normalizes the microphone level instead of using a fixed gain."); enabledWhen([this] { return draft.captureMicrophone; });
#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION
        check(L"Noise suppression", draft.audioInputNoiseSuppressionEnabled,
              L"Removes steady background noise from the microphone track."); enabledWhen([this] { return draft.captureMicrophone; });
#endif
        section(L"Sound separation");
        check(L"Exclude selected apps from system audio", draft.soundSeparationEnabled,
              L"Mutes the selected apps in system audio while they play. Best effort.");
        appList(true);
        break;
    case Category::Integrations:
        section(L"League of Legends");
        check(L"League of Legends kill reminder", game.leagueOfLegendsKillReminder,
              L"Tells you to save a replay within 6 seconds of a kill so the clip is tagged.");
        section(L"Voice command");
        enumChoice(L"Voice command", kVoiceCommandComboEntries, game.voiceCommand);
        section(L"Discord");
        enumChoice(L"Discord presence", kDiscordPresenceComboEntries, game.discordRichPresence,
                   L"Shows the foreground app as your Discord activity, with an elapsed timer.");
        choices(L"Discord timer", {{L"Reset on app switch", [&game] { game.discordRichPresenceResetTimerOnSwitch = true; }},
                                   {L"Keep running", [&game] { game.discordRichPresenceResetTimerOnSwitch = false; }}},
                                   game.discordRichPresenceResetTimerOnSwitch ? 0 : 1);
        enabledWhen([&game] { return game.discordRichPresence != GameIntegrationSettings::DiscordRichPresenceMode::Off; });
        section(L"Auto record");
        check(L"Record while a selected app is open", draft.autoRecordEnabled,
              L"Starts recording when one of these apps opens and stops when the last one closes.");
        appList(false);
        break;
    }
    save = button(L"Save settings", [this] { saveSettings(); }, settingsHelp, window);
    discard = button(L"Discard", [this] { discardSettings(); }, L"Revert unsaved settings changes.", window, ButtonTone::Quiet);
    if (discard) ShowWindow(discard, SW_HIDE);
}

void StudioWindow::Impl::appList(bool audio) {
    const size_t headerRow = rows.size();
    const auto isAdded = [this, audio](const std::filesystem::path& path) {
        const auto key = normalizedPathKey(path);
        const auto duplicate = [&key](const auto& item) { return normalizedPathKey(item.executablePath) == key; };
        return audio ? std::ranges::any_of(draft.soundSeparationApps, duplicate)
                     : std::ranges::any_of(draft.autoRecordApps, duplicate);
    };
    const auto add = [this, audio, headerRow, isAdded](std::wstring name, std::filesystem::path path, size_t focusCell) {
        if (path.empty()) return;
        if (auto invalid = invalidNumber()) {
            invalidSaveAttempt = invalid;
            SetFocus(invalid);
            RedrawWindow(invalid, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
            setStatus(L"Enter a valid number first.");
            return;
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) { setStatus(L"Select an existing executable."); return; }
        if (isAdded(path)) { setStatus(L"That app is already in the list."); return; }
        if (name.empty()) name = appNameFromPath(path);
        if (audio) draft.soundSeparationApps.push_back({std::move(name), std::move(path), true});
        else draft.autoRecordApps.push_back({std::move(name), std::move(path)});
        const int previous = scroll;
        buildPage();
        scrollTo(previous);
        if (headerRow < rows.size() && focusCell < rows[headerRow].cells.size())
            SetFocus(rows[headerRow].cells[focusCell]);
    };
    row({button(audio ? L"Add audio app\u2026" : L"Add open app\u2026", [this, audio, isAdded, add] {
        const auto apps = audio ? WasapiCapture::enumerateAudioSessionApps() : WasapiCapture::enumerateOpenApps();
        if (apps.empty()) { setStatus(L"No apps found. You can browse for an executable instead."); return; }
        const auto menu = CreatePopupMenu();
        if (!menu) return;
        for (size_t i = 0; i < apps.size(); ++i) {
            const auto label = apps[i].name.empty() ? appNameFromPath(apps[i].executablePath) : apps[i].name;
            const UINT flags = MF_STRING | (isAdded(apps[i].executablePath) ? (MF_CHECKED | MF_GRAYED) : 0);
            AppendMenuW(menu, flags, i + 1, label.c_str());
        }
        RECT anchor{};
        GetWindowRect(GetFocus(), &anchor);
        const auto selection = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, anchor.left, anchor.bottom, 0, window, nullptr);
        DestroyMenu(menu);
        if (selection > 0 && static_cast<size_t>(selection) <= apps.size())
            add(apps[selection - 1].name, apps[selection - 1].executablePath, 0);
    }), button(L"Browse for app\u2026", [this, add] {
        const COMDLG_FILTERSPEC exe{L"Executables", L"*.exe"};
        std::filesystem::path result;
        if (pickFileForOpen(window, L"Choose app", &exe, 1, L"exe", result)) add({}, std::move(result), 1);
    })});
    const size_t count = audio ? draft.soundSeparationApps.size() : draft.autoRecordApps.size();
    for (size_t i = 0; i < count; ++i) {
        const auto& name = audio ? draft.soundSeparationApps[i].name : draft.autoRecordApps[i].name;
        const auto& exePath = audio ? draft.soundSeparationApps[i].executablePath : draft.autoRecordApps[i].executablePath;
        const auto hint = exePath.wstring();
        const auto iconCell = make(L"STATIC", L"", WS_VISIBLE | SS_NOTIFY, {}, hint.c_str());
        if (iconCell) controls.at(iconCell).icon = loadExecutableIcon(exePath);
        const auto remove = button(L"Remove", [this, i, audio, headerRow] {
            if (const auto invalid = invalidNumber()) {
                invalidSaveAttempt = invalid;
                SetFocus(invalid);
                RedrawWindow(invalid, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
                setStatus(L"Enter a valid number first.");
                return;
            }
            if (audio) draft.soundSeparationApps.erase(draft.soundSeparationApps.begin() + i);
            else draft.autoRecordApps.erase(draft.autoRecordApps.begin() + i);
            const size_t remaining = audio ? draft.soundSeparationApps.size() : draft.autoRecordApps.size();
            const int previous = scroll;
            buildPage();
            scrollTo(previous);
            if (remaining > 0) {
                const size_t nextRow = headerRow + 1 + (std::min)(i, remaining - 1);
                if (nextRow < rows.size() && !rows[nextRow].cells.empty())
                    SetFocus(rows[nextRow].cells.back());
            } else if (headerRow < rows.size() && !rows[headerRow].cells.empty()) {
                SetFocus(rows[headerRow].cells[0]);
            }
        }, hint.c_str());
        if (audio) {
            const auto mute = checkbox((L"Mute " + name).c_str(), draft.soundSeparationApps[i].muted, hint.c_str());
            row({iconCell, mute, remove});
        } else {
            const auto appLabel = make(L"STATIC", name.c_str(),
                                       SS_CENTERIMAGE | SS_NOTIFY | SS_NOPREFIX | SS_ENDELLIPSIS | WS_VISIBLE, {}, hint.c_str());
            row({iconCell, appLabel, remove});
        }
        rows.back().widths = {24, 0, 76};
        rows.back().tail = true;
    }
}

} // namespace backtrack
