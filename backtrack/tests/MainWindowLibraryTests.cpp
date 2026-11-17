#include "ui/MainWindow.h"
#include "ui/UiHelpers.h"
#include "ui/UiIds.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace backtrack {

struct MainWindowLibraryTestAccess {
    static HWND window(const MainWindow& window) { return window.window_; }
    static HWND pageHost(const MainWindow& window) { return window.pageHost_; }
    static bool settingsDirty(const MainWindow& window) { return window.settingsDirty_; }
    static size_t page(const MainWindow& window) { return static_cast<size_t>(window.page_); }
    static void showPage(MainWindow& window, size_t index) {
        window.switchPage(static_cast<MainWindow::Page>(index));
    }
    static void stopControllerWorker(MainWindow& window) { window.stopControllerWorker(); }
    static void stopLibraryScan(MainWindow& window) { window.stopLibraryScanWorker(); }
    static void setScanOverride(
        MainWindow& window,
        std::function<std::vector<ClipInfo>(const std::filesystem::path&, const std::function<bool()>&)> scan) {
        window.libraryScanOverride_ = std::move(scan);
    }
    static bool scanPending(const MainWindow& window) { return window.libraryScanPending_; }
    static uint64_t scanGeneration(const MainWindow& window) {
        return window.libraryScanGeneration_.load(std::memory_order_acquire);
    }
    static std::filesystem::path clipDirectory(const MainWindow& window) { return window.clipsDirectory_; }
    static size_t clipsSize(const MainWindow& window) { return window.clips_.size(); }
    static void setClips(MainWindow& window, std::vector<ClipInfo> clips) {
        window.releaseClipThumbnailCache();
        window.clips_ = std::move(clips);
        window.clipThumbnails_.assign(window.clips_.size(), {});
        window.thumbnailFirst_ = window.thumbnailLast_ = 0;
    }
    static void setGallery(MainWindow& window) {
        window.libraryViewMode_ = MainWindow::LibraryViewMode::Gallery;
    }
    static void setViewMode(MainWindow& window, bool gallery) {
        window.setLibraryViewMode(gallery ? MainWindow::LibraryViewMode::Gallery : MainWindow::LibraryViewMode::List);
    }
    static bool isGallery(const MainWindow& window) {
        return window.libraryViewMode_ == MainWindow::LibraryViewMode::Gallery;
    }
    static void setSelectedClip(MainWindow& window, size_t index) { window.selectedClipIndex_ = index; }
    static size_t selectedClip(const MainWindow& window) { return window.selectedClipIndex_; }
    static std::filesystem::path clipPath(const MainWindow& window, size_t index) {
        return index < window.clips_.size() ? window.clips_[index].path : std::filesystem::path{};
    }
    static void populateRows(MainWindow& window) { window.populateClipListRows(); }
    static void suppressThumbnailWorker(MainWindow& window, bool suppressed) {
        window.thumbnailWorkerSuppressed_ = suppressed;
    }
    static void updateThumbnailWorkingSet(MainWindow& window) {
        window.updateThumbnailWorkingSet();
    }
    static size_t thumbnailQueueSize(MainWindow& window) {
        std::scoped_lock lock(window.thumbnailQueueMutex_);
        return window.thumbnailQueue_.size();
    }
    static size_t thumbnailCacheSize(MainWindow& window) {
        return window.thumbnailCache_.size();
    }
    static size_t thumbnailFirst(const MainWindow& window) { return window.thumbnailFirst_; }
    static size_t thumbnailLast(const MainWindow& window) { return window.thumbnailLast_; }
    static size_t thumbnailBudget() { return MainWindow::kMaxLibraryThumbnails; }
    static uint64_t thumbnailGeneration(const MainWindow& window) {
        return window.thumbnailGeneration_.load(std::memory_order_acquire);
    }
    static void completeThumbnail(MainWindow& window, size_t index, const ClipInfo& clip, uint64_t generation) {
        {
            std::scoped_lock lock(window.thumbnailQueueMutex_);
            window.thumbnailResult_.emplace(MainWindow::ThumbnailResult{
                generation, index, clip.path, clip.modifiedTime, clip.bytes, nullptr});
        }
        window.handleClipThumbnailReady();
    }
    static void injectScanResult(
        MainWindow& window, uint64_t generation, std::filesystem::path directory,
        std::vector<ClipInfo> clips) {
        std::scoped_lock lock(window.libraryScanMutex_);
        window.libraryScanResult_.emplace(MainWindow::LibraryScanResult{
            {generation, std::move(directory)}, std::move(clips), false});
    }
    static void handleScanReady(MainWindow& window) { window.handleLibraryScanReady(); }
};

} // namespace backtrack

namespace {

using namespace backtrack;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path uniqueRoot() {
    return std::filesystem::temp_directory_path() /
        (L"backtrack_ui_library_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount64()));
}

AppSettings testSettings(const std::filesystem::path& root) {
    AppSettings settings;
    settings.clipDirectory = root / L"clips";
    settings.checkForUpdates = false;
    settings.replay.enabled = false;
    settings.replay.diskBackedVideo = false;
    settings.replay.diskBackedAudio = false;
    settings.hotkeys.startStopVirtualKey = 0;
    settings.hotkeys.saveReplayVirtualKey = 0;
    return settings;
}

std::vector<ClipInfo> makeClips(const std::filesystem::path& directory, size_t count, const wchar_t* prefix = L"clip") {
    std::vector<ClipInfo> clips(count);
    for (size_t index = 0; index < count; ++index) {
        clips[index].path = directory / (std::wstring(prefix) + std::to_wstring(index) + L".mp4");
        clips[index].bytes = index + 1;
        clips[index].modifiedTime = std::filesystem::file_time_type{};
    }
    return clips;
}

struct Fixture {
    std::filesystem::path root;
    AppSettings settings;
    SettingsStore store;
    RecorderController controller;
    std::unique_ptr<MainWindow> window;

    Fixture()
        : root(uniqueRoot()),
          settings(testSettings(root)),
          store(root / L"settings.ini") {
        std::filesystem::create_directories(settings.clipDirectory);
        expect(controller.initialize(settings), "controller initialization failed");
        window = std::make_unique<MainWindow>(controller, store, settings, SettingsStore::LoadStatus::Missing);
        expect(window->create(GetModuleHandleW(nullptr), SW_SHOWNORMAL, false), "main window creation failed");
        MainWindowLibraryTestAccess::stopControllerWorker(*window);
    }

    ~Fixture() {
        if (!window) return;
        const HWND handle = MainWindowLibraryTestAccess::window(*window);
        if (handle && IsWindow(handle)) DestroyWindow(handle);
        window.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

struct Handles {
    HANDLE started = nullptr;
    HANDLE release = nullptr;
    ~Handles() {
        if (started) CloseHandle(started);
        if (release) CloseHandle(release);
    }
};

void testBlockedScanKeepsUiResponsive() {
    Handles handles{
        CreateEventW(nullptr, TRUE, FALSE, nullptr),
        CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    expect(handles.started && handles.release, "could not create scan events");
    Fixture fixture;

    MainWindowLibraryTestAccess::setScanOverride(*fixture.window,
        [&](const std::filesystem::path& directory, const std::function<bool()>& cancelled) {
            SetEvent(handles.started);
            while (WaitForSingleObject(handles.release, 20) == WAIT_TIMEOUT) {
                if (cancelled()) return std::vector<ClipInfo>{};
            }
            auto clips = makeClips(directory, 1, L"late");
            return clips;
        });

    MainWindowLibraryTestAccess::showPage(*fixture.window, 1);
    expect(WaitForSingleObject(handles.started, 5000) == WAIT_OBJECT_0, "library scan did not start");

    DWORD_PTR heartbeat = 0;
    expect(SendMessageTimeoutW(MainWindowLibraryTestAccess::window(*fixture.window), WM_NULL, 0, 0,
               SMTO_ABORTIFHUNG, 1000, &heartbeat) != 0,
           "UI heartbeat stalled behind a blocked library scan");

    MainWindowLibraryTestAccess::showPage(*fixture.window, 2);
    expect(MainWindowLibraryTestAccess::page(*fixture.window) == 2, "page change was blocked by the scan");
    expect(!MainWindowLibraryTestAccess::scanPending(*fixture.window), "page change left the scan pending");

    SetEvent(handles.release);
    MainWindowLibraryTestAccess::stopLibraryScan(*fixture.window);
    MSG message{};
    while (PeekMessageW(&message, MainWindowLibraryTestAccess::window(*fixture.window),
                        ui_ids::kLibraryScanReadyMessage, ui_ids::kLibraryScanReadyMessage, PM_REMOVE)) {
        DispatchMessageW(&message);
    }
    expect(MainWindowLibraryTestAccess::clipsSize(*fixture.window) == 0,
           "a scan cancelled by a page change replaced the clip snapshot");
}

void testStaleScanResultRejected() {
    Fixture fixture;
    MainWindowLibraryTestAccess::setScanOverride(*fixture.window,
        [](const std::filesystem::path&, const std::function<bool()>&) { return std::vector<ClipInfo>{}; });
    MainWindowLibraryTestAccess::showPage(*fixture.window, 1);
    MainWindowLibraryTestAccess::stopLibraryScan(*fixture.window);

    const auto directory = MainWindowLibraryTestAccess::clipDirectory(*fixture.window);
    expect(!directory.empty(), "library scan did not record the clip directory");
    MainWindowLibraryTestAccess::setClips(*fixture.window, makeClips(directory, 2, L"current"));
    const uint64_t generation = MainWindowLibraryTestAccess::scanGeneration(*fixture.window);

    MainWindowLibraryTestAccess::injectScanResult(
        *fixture.window, generation - 1, directory, makeClips(directory, 1, L"stale"));
    MainWindowLibraryTestAccess::handleScanReady(*fixture.window);
    expect(MainWindowLibraryTestAccess::clipsSize(*fixture.window) == 2,
           "a stale generation replaced the clip snapshot");

    MainWindowLibraryTestAccess::injectScanResult(
        *fixture.window, generation, fixture.root / L"elsewhere", makeClips(fixture.root / L"elsewhere", 1));
    MainWindowLibraryTestAccess::handleScanReady(*fixture.window);
    expect(MainWindowLibraryTestAccess::clipsSize(*fixture.window) == 2,
           "a scan for another directory replaced the clip snapshot");
}

void testViewSwitchKeepsSnapshotAndSelection() {
    Fixture fixture;
    MainWindowLibraryTestAccess::setScanOverride(*fixture.window,
        [](const std::filesystem::path&, const std::function<bool()>&) { return std::vector<ClipInfo>{}; });
    MainWindowLibraryTestAccess::showPage(*fixture.window, 1);
    MainWindowLibraryTestAccess::stopLibraryScan(*fixture.window);
    MainWindowLibraryTestAccess::suppressThumbnailWorker(*fixture.window, true);

    auto clips = makeClips(fixture.settings.clipDirectory, 3);
    MainWindowLibraryTestAccess::setClips(*fixture.window, clips);
    MainWindowLibraryTestAccess::setSelectedClip(*fixture.window, 1);
    const uint64_t generation = MainWindowLibraryTestAccess::scanGeneration(*fixture.window);

    MainWindowLibraryTestAccess::setViewMode(*fixture.window, true);
    expect(MainWindowLibraryTestAccess::isGallery(*fixture.window), "gallery view was not applied");
    expect(MainWindowLibraryTestAccess::clipsSize(*fixture.window) == 3, "gallery switch dropped the clip snapshot");
    expect(MainWindowLibraryTestAccess::selectedClip(*fixture.window) == 1, "gallery switch changed the selection");
    expect(MainWindowLibraryTestAccess::clipPath(*fixture.window, 0) == clips[0].path &&
               MainWindowLibraryTestAccess::clipPath(*fixture.window, 1) == clips[1].path &&
               MainWindowLibraryTestAccess::clipPath(*fixture.window, 2) == clips[2].path,
           "gallery switch changed clip ordering");
    expect(MainWindowLibraryTestAccess::scanGeneration(*fixture.window) == generation,
           "gallery switch started a library rescan");

    MainWindowLibraryTestAccess::setViewMode(*fixture.window, false);
    expect(!MainWindowLibraryTestAccess::isGallery(*fixture.window), "list view was not applied");
    expect(MainWindowLibraryTestAccess::clipsSize(*fixture.window) == 3 &&
               MainWindowLibraryTestAccess::selectedClip(*fixture.window) == 1,
           "list switch changed the snapshot or selection");
    expect(MainWindowLibraryTestAccess::scanGeneration(*fixture.window) == generation,
           "list switch started a library rescan");
}

void testThumbnailCompletionsStayBounded() {
    Fixture fixture;
    MainWindowLibraryTestAccess::setScanOverride(*fixture.window,
        [](const std::filesystem::path&, const std::function<bool()>&) { return std::vector<ClipInfo>{}; });
    MainWindowLibraryTestAccess::showPage(*fixture.window, 1);
    MainWindowLibraryTestAccess::stopLibraryScan(*fixture.window);

    MainWindowLibraryTestAccess::setGallery(*fixture.window);
    MainWindowLibraryTestAccess::suppressThumbnailWorker(*fixture.window, true);
    constexpr size_t kClipCount = 5000;
    auto clips = makeClips(fixture.settings.clipDirectory, kClipCount);
    MainWindowLibraryTestAccess::setClips(*fixture.window, clips);
    MainWindowLibraryTestAccess::populateRows(*fixture.window);
    MainWindowLibraryTestAccess::updateThumbnailWorkingSet(*fixture.window);

    const size_t budget = MainWindowLibraryTestAccess::thumbnailBudget();
    expect(MainWindowLibraryTestAccess::thumbnailQueueSize(*fixture.window) <= budget,
           "thumbnail requests were not bounded by the working-set budget");
    expect(MainWindowLibraryTestAccess::thumbnailLast(*fixture.window) -
               MainWindowLibraryTestAccess::thumbnailFirst(*fixture.window) <= budget,
           "thumbnail working set exceeded the fixed budget");

    const uint64_t generation = MainWindowLibraryTestAccess::thumbnailGeneration(*fixture.window);
    for (size_t index = 0; index < kClipCount; ++index) {
        MainWindowLibraryTestAccess::completeThumbnail(*fixture.window, index, clips[index], generation);
    }
    expect(MainWindowLibraryTestAccess::thumbnailCacheSize(*fixture.window) <= budget,
           "thousands of thumbnail completions grew the cache past the fixed budget");
}

void testSettingsRebuildNotifications() {
    Fixture fixture;
    auto& window = *fixture.window;
    MainWindowLibraryTestAccess::showPage(window, 2);
    const HWND root = MainWindowLibraryTestAccess::window(window);
    const struct {
        int category;
        int id;
        int notification;
        const wchar_t* on;
        const wchar_t* off;
        int dependent;
    } cases[] = {
        {2, kResolutionModeComboId, CBN_SELCHANGE, L"Custom size", L"1080p", kWidthEditId},
        {4, kDiscordRichPresenceComboId, CBN_SELCHANGE, L"Always", L"Never", kDiscordPresenceTimerComboId},
        {1, kMultiMonitorSupportCheckId, BN_CLICKED, nullptr, nullptr, kMonitorFollowComboId},
        {2, kEncoderProfileComboId, CBN_SELCHANGE, L"Custom", L"Balanced", kEncoderPresetComboId},
        {1, kReplayDiskBackedCheckId, BN_CLICKED, nullptr, nullptr, kReplayRingDirEditId},
        {1, kReplayDiskBackedAudioCheckId, BN_CLICKED, nullptr, nullptr, kReplayRingDirEditId},
    };
    for (const auto& test : cases) {
        SendMessageW(root, WM_COMMAND, MAKEWPARAM(kSettingsCategoryButtonBaseId + test.category, BN_CLICKED), 0);
        const HWND host = MainWindowLibraryTestAccess::pageHost(window);
        for (bool enabled : {false, true, false}) {
            const HWND control = GetDlgItem(host, test.id);
            expect(control != nullptr, "rebuild trigger control is missing");
            if (test.notification == CBN_SELCHANGE) {
                const LRESULT index = SendMessageW(control, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                    reinterpret_cast<LPARAM>(enabled ? test.on : test.off));
                expect(index != CB_ERR, "rebuild trigger combo entry is missing");
                SendMessageW(control, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
            } else {
                SendMessageW(control, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            if (enabled) {
                SendMessageW(root, WM_COMMAND, MAKEWPARAM(test.id, EN_KILLFOCUS), reinterpret_cast<LPARAM>(control));
                expect(!GetDlgItem(host, test.dependent), "unrelated notification rebuilt the category");
            }
            SendMessageW(root, WM_COMMAND, MAKEWPARAM(test.id, test.notification), reinterpret_cast<LPARAM>(control));
            expect((GetDlgItem(host, test.dependent) != nullptr) == enabled,
                   "rebuild notification did not preserve the edited value and update dependent controls");
            if (enabled) {
                expect(MainWindowLibraryTestAccess::settingsDirty(window), "rebuild notification did not mark settings dirty");
            }
        }
    }
}

struct BitmapSurface {
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    uint32_t* pixels = nullptr;

    BitmapSurface(int width, int height) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        pixels = static_cast<uint32_t*>(bits);
        if (dc && bitmap) oldBitmap = SelectObject(dc, bitmap);
    }
    ~BitmapSurface() {
        if (oldBitmap) SelectObject(dc, oldBitmap);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
};

void testBitmapScalingReplacesOnlyOnSuccess() {
    BitmapSurface source(4, 2);
    BitmapSurface target(8, 8);
    expect(source.oldBitmap && target.oldBitmap && source.pixels && target.pixels, "could not create bitmap surfaces");
    std::fill_n(source.pixels, 8, 0x00336699u);
    SelectObject(source.dc, source.oldBitmap);
    struct Cache {
        HBITMAP bitmap = nullptr;
        ~Cache() { if (bitmap) DeleteObject(bitmap); }
    } cache;
    int width = 0;
    int height = 0;
    drawBitmapFitCached(target.dc, source.bitmap, cache.bitmap, width, height, RECT{0, 0, 8, 8});
    expect(cache.bitmap && width == 8 && height == 4, "bitmap scaling did not preserve aspect ratio");
    GdiFlush();
    expect((target.pixels[4 * 8 + 4] & 0x00ffffffu) == 0x00336699u, "scaled bitmap changed the source color");

    const HBITMAP previous = cache.bitmap;
    drawBitmapFitCached(target.dc, source.bitmap, cache.bitmap, width, height, RECT{0, 0, 4, 4});
    BITMAP info{};
    expect(cache.bitmap != previous && width == 4 && height == 2, "successful scaling did not replace the cached bitmap");
    expect(GetObjectW(previous, sizeof(info), &info) == 0, "successful replacement leaked the previous bitmap");

    // a bitmap already selected in another DC cannot be selected by the scaler.
    SelectObject(source.dc, source.bitmap);
    const HBITMAP retained = cache.bitmap;
    drawBitmapFitCached(target.dc, source.bitmap, cache.bitmap, width, height, RECT{0, 0, 6, 6});
    expect(cache.bitmap == retained && width == 4 && height == 2, "failed scaling replaced the existing cache");
    expect(GetObjectW(cache.bitmap, sizeof(info), &info) == sizeof(info), "failed scaling deleted the existing cache");
    expect(GetObjectW(source.bitmap, sizeof(info), &info) == sizeof(info) && info.bmWidth == 4 && info.bmHeight == 2,
           "failed scaling destroyed or changed the original bitmap");
    expect(GetCurrentObject(source.dc, OBJ_BITMAP) == source.bitmap, "scaling changed the source DC's selected bitmap");
}

} // namespace

int main() {
    try {
        const HRESULT com = OleInitialize(nullptr);
        expect(SUCCEEDED(com), "OleInitialize failed");
        struct OleCleanup { ~OleCleanup() { OleUninitialize(); } } ole;
        testBlockedScanKeepsUiResponsive();
        testStaleScanResultRejected();
        testViewSwitchKeepsSnapshotAndSelection();
        testThumbnailCompletionsStayBounded();
        testSettingsRebuildNotifications();
        testBitmapScalingReplacesOnlyOnSuccess();
        std::cout << "MainWindowLibraryTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MainWindowLibraryTests failed: " << error.what() << '\n';
        return 1;
    }
}
