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

} // namespace

HWND StudioWindow::Impl::editField(const std::wstring& value, DWORD style, ControlEvent onChange, const wchar_t* help) {
    const auto field = make(L"EDIT", value.c_str(), WS_TABSTOP | WS_VISIBLE | WS_BORDER | style, std::move(onChange), help);
    SendMessageW(field, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(5), px(5)));
    return field;
}

HWND StudioWindow::Impl::edit(const wchar_t* label, const std::wstring& value, DWORD style,
                              ControlEvent onChange, const wchar_t* help) {
    auto title = make(L"STATIC", label, SS_CENTERIMAGE | SS_NOTIFY | SS_NOPREFIX | WS_VISIBLE, {}, help);
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
    return editField(std::to_wstring(value), ES_NUMBER | ES_AUTOHSCROLL,
        [this, &value, &fallback](HWND hwnd, UINT event) {
            if (event != EN_CHANGE) return;
            const auto parsed = parseUIntControl(hwnd);
            if (!parsed) {
                const bool reset = std::exchange(value, fallback) != fallback;
                if (invalidNumbers.insert(hwnd).second || reset) changed();
                return;
            }
            const bool wasInvalid = invalidNumbers.erase(hwnd) != 0;
            if (std::exchange(value, *parsed) != *parsed || wasInvalid) changed();
        }, help);
}

void StudioWindow::Impl::number(const wchar_t* title, uint32_t& value, const uint32_t& fallback, const wchar_t* help) {
    auto label = make(L"STATIC", title, SS_CENTERIMAGE | SS_NOTIFY | SS_NOPREFIX | WS_VISIBLE, {}, help);
    row({label, numberField(value, fallback, help)}, true);
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
    const auto label = make(L"STATIC", title, SS_CENTERIMAGE | SS_NOTIFY | SS_NOPREFIX | WS_VISIBLE, {}, help);
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
                            SS_CENTERIMAGE | SS_NOTIFY | SS_NOPREFIX | WS_VISIBLE, {}, help);
    const auto combo = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST |
        CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, [this, index, &id](HWND hwnd, UINT event) {
            const auto& device = deviceCombos[index];
            if (event == CBN_SELCHANGE) {
                const auto selected = SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
                if (selected >= 0 && static_cast<size_t>(selected) < device.ids.size()) {
                    id = device.ids[selected];
                    changed();
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
    static const std::vector<AudioDeviceInfo> noEndpoints;
    auto list = deviceChoices(deviceWorker.joinable() ? noEndpoints : deviceEndpoints[index], id);
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
    if (save) {
        const bool enabled = dirty();
        if ((IsWindowEnabled(save) != FALSE) != enabled) EnableWindow(save, enabled);
    }
}

bool StudioWindow::Impl::saveSettings(bool refreshPage) {
    if (const auto invalid = invalidNumber()) {
        SetFocus(invalid);
        setStatus(L"Enter a whole number between 0 and 4,294,967,295.");
        return false;
    }
    if (busy || shuttingDown) { setStatus(L"Recorder is busy. Try saving again when it finishes."); return false; }
    if (!readable) { setStatus(L"Settings could not be read at startup. Restart after the file becomes readable."); return false; }
    const auto candidate = sanitizeSettings(draft);
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
        if (const auto error = publishWindowSettings(store, controller, saved, candidate, readable)) {
            setStatus(error);
            return false;
        }
        ControllerAction settings;
        settings.kind = ControllerActionKind::ApplySettings;
        settings.settings = saved;
        settings.hotkeysOk = hotkeys.registerHotkeys(window, saved.hotkeys);
        settings.hotkeyError = hotkeys.lastErrorMessage();
        submit(std::move(settings), L"Settings saved. Applying capture changes\u2026");
    }
    const bool normalized = draft != saved;
    if (normalized) draft = saved;
    if (refreshPage) {
        if (normalized || !invalidNumbers.empty()) buildPage();
        else changed();
        SetFocus(nav.page == Page::Settings ? categoryButtons[static_cast<size_t>(nav.category)] : pageButtons[0]);
    }
    return true;
}

bool StudioWindow::Impl::leaveSettings(bool refreshPage) {
    if (!dirty()) return true;
    const int answer = MessageBoxW(window, L"Save your changes before leaving?", L"Unsaved settings", MB_ICONWARNING | MB_YESNOCANCEL);
    if (answer == IDYES) return saveSettings(refreshPage);
    if (answer != IDNO) return false;
    draft = saved;
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
    const auto compact = [this](decltype(Row::widths) widths) {
        rows.back().inset = studio_theme::sectionInset;
        rows.back().widths = widths;
    };

    section(L"Recording");
    auto& keys = draft.hotkeys;
    // Rebuilding the page must not expose an action that is still finalizing.
    record = button(motions[recordGlyph].to != 0.f ? L"Stop recording" : L"Start recording", [this] {
        submit(ControllerActionKind::ToggleRecording,
               controller.isRecording() ? L"Stopping recording\u2026" : L"Starting recording\u2026", clickOrigin == record);
    }, L"", nullptr, ButtonTone::Primary);
    if (record) controls.at(record).pointerMotion = true;
    row({record, hotkeyBox(keys.startStopModifiers, keys.startStopVirtualKey)});
    compact({actionWidth, 164});

    recover = button(L"Recover failed recording", [this] {
        submit(ControllerActionKind::RecoverFailedRecording, L"Recovering recording\u2026");
    }, L"", nullptr, ButtonTone::Quiet);
    recoveryRow = rows.size();
    row({recover}, false, 28);
    compact({176});
    syncRecoveryAction();

    section(L"Instant replay");
    row({checkbox(L"Enable replay", draft.replay.enabled, L""),
         hotkeyBox(keys.saveReplayModifiers, keys.saveReplayVirtualKey)});
    compact({136, 164});
    visibleWhen(rows.back().cells[1], [this] { return draft.replay.enabled; });
    replayButton = button(L"Save", [this] { requestReplay(clickOrigin == replayButton); },
                          L"", nullptr, ButtonTone::Primary);
    if (replayButton) controls.at(replayButton).pointerMotion = true;
    row({replayButton, make(L"STATIC", L"the last", SS_CENTERIMAGE | WS_VISIBLE),
         numberField(draft.replay.seconds, saved.replay.seconds, secondsHelp),
         make(L"STATIC", L"seconds", SS_CENTERIMAGE | WS_VISIBLE)});
    compact({80, 54, 60, 64});
    enabledWhen([this] { return draft.replay.enabled; }, 0);

    save = button(L"Save settings", [this] { saveSettings(); }, settingsHelp, window);
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
    case Category::App:
        section(L"Application");
        check(L"Start minimized with Windows", draft.startWithWindowsMinimized,
              L"Adds Backtrack to Windows startup. It starts minimized only when Windows launches it.");
        check(L"Close to the tray", draft.exitToTray, L"Closing the window leaves Backtrack running. Use Exit on the tray icon to quit.");
        check(L"Check for updates at startup", draft.checkForUpdates, L"Asks GitHub for a new release once per launch.");
        break;
    case Category::Capture: {
        constexpr int labelWidth = 152;
        constexpr int controlInset = studio_theme::sectionInset + labelWidth + studio_theme::gap;
        const auto compact = [this](decltype(Row::widths) widths, int inset = studio_theme::sectionInset) {
            auto& item = rows.back();
            assert(item.cells.size() <= widths.size());
            item.inset = inset;
            item.widths = widths;
            item.height = 32;
        };

        section(L"Capture");
        path(L"Clip folder", draft.clipDirectory, true);
        compact({labelWidth, 256, 88});
        check(L"Organize clips by game", draft.organizeClipsByGame,
              L"Saves replays in a folder named after the game. Manual recordings stay in the clip folder.");
        compact({204}, controlInset);
        enumChoice(L"Capture method", kCaptureBackendComboEntries, draft.preferredCaptureBackend,
                   L"WGC is the default. Duplication is tried first on HDR displays and whenever WGC fails.");
        compact({labelWidth, 204});
        check(L"Capture mouse cursor", draft.captureCursor);
        compact({204}, controlInset);
        const auto storage = make(L"STATIC", L"Store on disk", SS_CENTERIMAGE | SS_NOTIFY | WS_VISIBLE, {},
                                  L"Keeps the buffered replay on the drive instead of in memory.");
        row({storage, checkbox(L"Video", draft.replay.diskBackedVideo, L""),
                      checkbox(L"Audio", draft.replay.diskBackedAudio, L"")});
        compact({labelWidth, 76, 76});
        path(L"Buffer folder", draft.replayRingDirectory, true, L"Leave blank to keep the buffer files in the clip folder.");
        compact({labelWidth - studio_theme::dependencyIndent, 256, 88});
        enabledWhen([this] { return draft.replay.diskBackedVideo || draft.replay.diskBackedAudio; });
        number(L"Clip overlap", draft.replay.overlapSeconds, saved.replay.overlapSeconds,
               L"When saving again within the replay window, save only new footage plus this many seconds of the previous clip.");
        rows.back().cells.push_back(make(L"STATIC", L"seconds", SS_CENTERIMAGE | WS_VISIBLE));
        compact({labelWidth, 60, 64});

        section(L"Multi-monitor");
        check(L"Follow across monitors", draft.multiMonitorSupport,
              L"Keeps capture on the monitor holding the focused app or the pointer.");
        compact({204}, controlInset);
        choices(L"Follow", {{L"Focused monitor", [this] { draft.followMouseMonitor = false; }},
                            {L"Mouse pointer", [this] { draft.followMouseMonitor = true; }}}, draft.followMouseMonitor ? 1 : 0);
        compact({labelWidth - studio_theme::dependencyIndent, 204});
        const auto multiMonitor = [this] { return draft.multiMonitorSupport; };
        enabledWhen(multiMonitor);
        check(L"Stabilize frames across monitors", draft.gpu.stableMultimonitorFrames,
              L"Copies every frame so a monitor switch cannot resize a texture the encoder still reads.");
        compact({256});
        enabledWhen(multiMonitor);
        save = button(L"Save", [this] { saveSettings(); }, settingsHelp, window);
        return;
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
        number(L"Width", video.width, saved.video.width); resolutionFields[0] = lastField();
        enabledWhen([&video] { return video.resolutionMode == ResolutionMode::Custom; });
        number(L"Height", video.height, saved.video.height); resolutionFields[1] = lastField();
        enabledWhen([&video] { return video.resolutionMode == ResolutionMode::Custom; });
        number(L"Frame rate (fps)", video.fps, saved.video.fps);
        number(L"Bitrate (kbps)", video.bitrateKbps, saved.video.bitrateKbps);
        section(L"GPU");
        enumChoice(L"Adaptive mode", kGpuAdaptiveComboEntries, draft.gpu.adaptiveMode,
                   L"Drops frames when the encoder falls behind. Aggressive drops earlier than Conservative.");
        number(L"Frame queue limit", draft.gpu.frameQueueLimit, saved.gpu.frameQueueLimit,
               L"Captured frames allowed to wait for the encoder. A deeper queue smooths short stalls and adds latency.");
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
               L"Frames the encoder may hold back, up to 31.");
        enabledWhen([&video, custom] { return custom() && video.encoderLookahead; }, 2);
        check(L"Adaptive I-frames", video.encoderAdaptiveIFrames); enabledWhen([&video, custom] { return custom() && video.encoderLookahead; }, 2);
        check(L"Spatial adaptive quantization", video.encoderSpatialAQ); enabledWhen(custom);
        number(L"Spatial AQ strength", video.encoderAQStrength, saved.video.encoderAQStrength);
        enabledWhen([&video, custom] { return custom() && video.encoderSpatialAQ; }, 2);
        check(L"Temporal adaptive quantization", video.encoderTemporalAQ); enabledWhen(custom);
        check(L"B-frames", video.encoderBFrames); enabledWhen(custom);
        check(L"Adaptive B-frames", video.encoderAdaptiveBFrames);
        enabledWhen([&video, custom] { return custom() && video.encoderBFrames && video.encoderLookahead; }, 2);
        number(L"Keyframe interval (seconds)", video.gopSeconds, saved.video.gopSeconds,
               L"Maximum seconds between keyframes. A replay save starts at a keyframe, so a shorter interval trims more precisely.");
        number(L"Reference frames", video.encoderReferenceFrames, saved.video.encoderReferenceFrames); enabledWhen(custom);
        check(L"Zero reorder delay", video.encoderZeroReorderDelay,
              L"Encodes frames in capture order. Turning it off adds latency and stops idle frames from being coalesced."); enabledWhen(custom);
        break;
    }
    case Category::Audio:
        section(L"Notifications");
        number(L"Volume (%)", draft.notificationSoundVolumePercent, saved.notificationSoundVolumePercent);
        path(L"Replay success sound", draft.customNotificationSoundPath, false, L"Choose a WAV file or leave blank for the built-in sound.");
        section(L"Devices");
        check(L"Capture system audio", draft.captureSystemAudio);
        device(AudioTrack::System, draft.audioOutputDeviceId); enabledWhen([this] { return draft.captureSystemAudio; });
        number(L"Desktop gain (%)", draft.audioOutputVolumePercent, saved.audioOutputVolumePercent);
        enabledWhen([this] { return draft.captureSystemAudio; });
        check(L"Capture microphone", draft.captureMicrophone);
        device(AudioTrack::Microphone, draft.audioInputDeviceId); enabledWhen([this] { return draft.captureMicrophone; });
        number(L"Microphone gain (%)", draft.audioInputVolumePercent, saved.audioInputVolumePercent);
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
}

void StudioWindow::Impl::appList(bool audio) {
    const auto add = [this, audio](std::wstring name, std::filesystem::path path) {
        if (path.empty()) return;
        if (auto invalid = invalidNumber()) { SetFocus(invalid); setStatus(L"Enter a valid number first."); return; }
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) { setStatus(L"Select an existing executable."); return; }
        const auto key = normalizedPathKey(path);
        const auto duplicate = [&key](const auto& item) { return normalizedPathKey(item.executablePath) == key; };
        if (name.empty()) name = appNameFromPath(path);
        if (audio) {
            if (std::ranges::any_of(draft.soundSeparationApps, duplicate)) return;
            draft.soundSeparationApps.push_back({std::move(name), std::move(path), true});
        } else {
            if (std::ranges::any_of(draft.autoRecordApps, duplicate)) return;
            draft.autoRecordApps.push_back({std::move(name), std::move(path)});
        }
        const int previous = scroll;
        buildPage();
        scrollTo(previous);
        SetFocus(save);
    };
    row({button(audio ? L"Add audio app\u2026" : L"Add open app\u2026", [this, audio, add] {
        const auto apps = audio ? WasapiCapture::enumerateAudioSessionApps() : WasapiCapture::enumerateOpenApps();
        if (apps.empty()) { setStatus(L"No apps found. You can browse for an executable instead."); return; }
        const auto menu = CreatePopupMenu();
        if (!menu) return;
        for (size_t i = 0; i < apps.size(); ++i) {
            const auto label = apps[i].name.empty() ? appNameFromPath(apps[i].executablePath) : apps[i].name;
            AppendMenuW(menu, MF_STRING, i + 1, label.c_str());
        }
        RECT anchor{};
        GetWindowRect(GetFocus(), &anchor);
        const auto selection = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, anchor.left, anchor.bottom, 0, window, nullptr);
        DestroyMenu(menu);
        if (selection > 0 && static_cast<size_t>(selection) <= apps.size()) add(apps[selection - 1].name, apps[selection - 1].executablePath);
    }), button(L"Browse for app\u2026", [this, add] {
        const COMDLG_FILTERSPEC exe{L"Executables", L"*.exe"};
        std::filesystem::path result;
        if (pickFileForOpen(window, L"Choose app", &exe, 1, L"exe", result)) add({}, std::move(result));
    })});
    const size_t count = audio ? draft.soundSeparationApps.size() : draft.autoRecordApps.size();
    for (size_t i = 0; i < count; ++i) {
        const auto& name = audio ? draft.soundSeparationApps[i].name : draft.autoRecordApps[i].name;
        const auto hint = audio ? draft.soundSeparationApps[i].executablePath.wstring() : draft.autoRecordApps[i].executablePath.wstring();
        const auto remove = button((L"Remove " + name).c_str(), [this, i, audio] {
            if (const auto invalid = invalidNumber()) { SetFocus(invalid); setStatus(L"Enter a valid number first."); return; }
            if (audio) draft.soundSeparationApps.erase(draft.soundSeparationApps.begin() + i);
            else draft.autoRecordApps.erase(draft.autoRecordApps.begin() + i);
            const int previous = scroll;
            buildPage();
            scrollTo(previous);
            SetFocus(save);
        }, hint.c_str());
        if (audio) {
            check((L"Mute " + name).c_str(), draft.soundSeparationApps[i].muted, hint.c_str());
            rows.back().cells.push_back(remove);
        } else row({remove});
    }
}

} // namespace backtrack
