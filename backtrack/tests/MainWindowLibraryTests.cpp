#include "ui/MainWindow.h"
#include "ui/UiIds.h"

#include <Windows.h>
#include <objbase.h>

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
    static size_t thumbnailSlotCount(const MainWindow& window) { return window.clipThumbnails_.size(); }
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
    expect(MainWindowLibraryTestAccess::thumbnailSlotCount(*fixture.window) == kClipCount,
           "thumbnail slots no longer cover the clip snapshot");
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
        std::cout << "MainWindowLibraryTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MainWindowLibraryTests failed: " << error.what() << '\n';
        return 1;
    }
}
