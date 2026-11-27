#include "studio/StudioWindowImpl.h"
#include "studio/GalleryLayout.h"
#include "studio/StudioTheme.h"

#include "core/LogContext.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"
#include "ui_common/WindowTools.h"

#include <CommCtrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdlib>
#include <deque>

namespace backtrack {
namespace {
namespace theme = studio_theme;
constexpr size_t noClip = static_cast<size_t>(-1);
constexpr size_t thumbnailBudget = 48;
static_assert(StudioGalleryLayout{3, 100}.visible(20, 150, 0, 1) == std::pair<size_t, size_t>{0, 0});

struct Bitmap {
    HBITMAP source = nullptr;
    HBITMAP scaled = nullptr;
    int width = 0;
    int height = 0;
    explicit Bitmap(HBITMAP handle) : source(handle) {}
    Bitmap(Bitmap&& other) noexcept
        : source(std::exchange(other.source, nullptr)), scaled(std::exchange(other.scaled, nullptr)),
          width(other.width), height(other.height) {}
    Bitmap& operator=(Bitmap&& other) noexcept {
        std::swap(source, other.source);
        std::swap(scaled, other.scaled);
        std::swap(width, other.width);
        std::swap(height, other.height);
        return *this;
    }
    ~Bitmap() { if (source) DeleteObject(source); if (scaled) DeleteObject(scaled); }
};

struct Thumbnail {
    uintmax_t bytes = 0;
    std::filesystem::file_time_type modifiedTime{};
    int width = 0;
    Bitmap bitmap{nullptr};
};

// Decode in 160px steps so live resizing does not re-decode on every pixel.
int thumbnailWidth(int tileWidth) {
    return ((std::max)(tileWidth, 1) + 159) / 160 * 160;
}

struct GalleryClip {
    ClipInfo info;
    std::wstring name;
    std::wstring duration;
    std::wstring details;
};

struct GalleryFolder {
    std::wstring name;
    std::filesystem::path path;
    size_t count = 0;
};

bool sameMedia(const ClipInfo& a, const ClipInfo& b) {
    return a.bytes == b.bytes && a.modifiedTime == b.modifiedTime && a.path == b.path;
}

bool shellDrag(HWND hwnd, const std::filesystem::path& path) {
    PIDLIST_ABSOLUTE absolute = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &absolute, 0, nullptr)) || !absolute) return false;
    auto parent = ILCloneFull(absolute);
    Microsoft::WRL::ComPtr<IDataObject> object;
    PCUITEMID_CHILD child = ILFindLastID(absolute);
    if (parent && child && ILRemoveLastID(parent))
        SHCreateDataObject(parent, 1, &child, nullptr, IID_PPV_ARGS(&object));
    CoTaskMemFree(parent);
    CoTaskMemFree(absolute);
    if (!object) return false;
    DWORD effect = DROPEFFECT_NONE;
    // The shell's default drop source adds the thumbnail drag image.
    return SUCCEEDED(SHDoDragDrop(hwnd, object.Get(), nullptr, DROPEFFECT_COPY, &effect));
}
} // namespace

struct StudioWindow::Impl::Gallery {
    struct Tile { HWND hwnd; size_t index = noClip; };
    struct Viewport {
        int width, height, scroll;
        unsigned dpi;
        bool operator==(const Viewport&) const = default;
    };
    struct Scan { uint64_t revision; std::filesystem::path directory; bool includeHiddenFolders = false; };
    struct Snapshot { uint64_t revision; std::vector<GalleryClip> clips; std::vector<GalleryFolder> folders; bool ok = false; };
    using PreviewKey = std::pair<uint64_t, size_t>;
    struct Request {
        uint64_t revision;
        size_t index;
        ClipInfo info;
        int width;
        PreviewKey key() const { return {revision, index}; }
    };
    struct Preview { Request request; Bitmap bitmap; };
    struct Edit {
        ClipEdit kind;
        std::filesystem::path directory;
        std::filesystem::path path;
        std::wstring name;
        bool favorite;
    };
    struct Edited { Edit edit; bool ok; };

    explicit Gallery(HWND root) : worker([this, root](std::stop_token stop) { run(root, stop); }) {}

    void resetPreviews() {
        std::scoped_lock lock(mutex);
        ++previewRevision;
        first = last = 0;
        requests.clear();
        previews.clear();
    }

    void cancelDrag() {
        if (const auto tile = std::exchange(dragTile, nullptr); tile && GetCapture() == tile) ReleaseCapture();
    }

    HWND tileFor(size_t index) const {
        if (index == noClip || tiles.empty()) return nullptr;
        const auto& tile = tiles[index % tiles.size()];
        return tile.index == index ? tile.hwnd : nullptr;
    }

    bool scrollTo(int64_t position) {
        const auto next = static_cast<int>(std::clamp<int64_t>(position, 0, maxScroll));
        return std::exchange(scroll, next) != next;
    }

    void run(HWND root, std::stop_token stop) {
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            setThreadDescriptionSafe(L"Backtrack library");
            LogContext::ScopedTask task(L"library");
            const auto postReady = [&] {
                if (!noticePending && !stop.stop_requested()) noticePending = PostMessageW(root, galleryReady, 0, 0) != FALSE;
            };
            ClipManager manager;
            for (;;) {
                std::optional<Scan> nextScan;
                std::optional<Request> thumbnail;
                std::optional<Edit> nextEdit;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, stop, [this] {
                        // The UI may replace the clip list when it consumes this result.
                        return edit || scan || (!requests.empty() && !(noticePending && (snapshot || edited)));
                    });
                    if (stop.stop_requested() && !edit) break;
                    if (edit) nextEdit = std::exchange(edit, std::nullopt);
                    else if (scan) nextScan = std::exchange(scan, std::nullopt);
                    else {
                        thumbnail = std::move(requests.front());
                        requests.pop_front();
                        decoding = thumbnail->key();
                    }
                }
                if (nextEdit) {
                    bool ok = false;
                    try {
                        manager.setDirectory(nextEdit->directory);
                        switch (nextEdit->kind) {
                        case ClipEdit::Favorite: ok = manager.setFavorite(nextEdit->path, nextEdit->favorite); break;
                        case ClipEdit::Remove: ok = manager.removeClip(nextEdit->path); break;
                        case ClipEdit::Rename: ok = manager.renameClip(nextEdit->path, nextEdit->name); break;
                        }
                    } catch (...) { Logger::instance().warning(L"ui", L"Clip edit failed unexpectedly"); }
                    std::scoped_lock lock(mutex);
                    edited = Edited{std::move(*nextEdit), ok};
                    postReady();
                } else if (nextScan) {
                    const auto cancelled = [&] { return stop.stop_requested() || revision.load() != nextScan->revision; };
                    std::optional<Snapshot> result{Snapshot{nextScan->revision}};
                    try {
                        if (cancelled()) continue;
                        manager.setDirectory(nextScan->directory);
                        auto contents = manager.listContents(cancelled, nextScan->includeHiddenFolders);
                        if (cancelled()) continue;
                        result->clips.reserve(contents.clips.size());
                        for (auto& info : contents.clips) {
                            if (cancelled()) break;
                            auto name = info.path.filename().wstring();
                            auto duration = durationToText(info.duration100ns);
                            auto details = info.duration100ns > 0
                                ? bytesToText(info.bytes)
                                : duration + L"  \u00b7  " + bytesToText(info.bytes);
                            if (const auto date = shortDateText(info.modifiedTime); !date.empty()) details += L"  \u00b7  " + date;
                            if (!info.tags.empty()) details += L"  \u00b7  " + tagsToText(info.tags);
                            result->clips.push_back({std::move(info), std::move(name), std::move(duration), std::move(details)});
                        }
                        for (auto& folder : contents.folders)
                            result->folders.push_back({std::move(folder.name), std::move(folder.path), folder.clips});
                        result->ok = true;
                    } catch (...) { Logger::instance().warning(L"ui", L"Library scan failed unexpectedly"); }
                    std::scoped_lock lock(mutex);
                    if (cancelled()) continue;
                    snapshot.swap(result);
                    postReady();
                } else {
                    HBITMAP source = nullptr;
                    if (!stop.stop_requested()) {
                        try { source = loadShellThumbnail(thumbnail->info.path, thumbnail->width, thumbnail->width * 9 / 16); }
                        catch (...) { Logger::instance().warning(L"ui", L"Thumbnail decode failed unexpectedly"); }
                    }
                    Bitmap bitmap(source);
                    std::scoped_lock lock(mutex);
                    decoding.reset();
                    if (stop.stop_requested() || thumbnail->revision != previewRevision ||
                        thumbnail->index < first || thumbnail->index >= last) continue;
                    assert(previews.size() < thumbnailBudget);
                    previews.push_back({std::move(*thumbnail), std::move(bitmap)});
                    postReady();
                }
            }
        } catch (...) { Logger::instance().error(L"ui", L"Library worker stopped after an internal error"); }
        if (SUCCEEDED(com)) CoUninitialize();
    }

    HWND view = nullptr;
    HWND breadcrumb = nullptr;
    HWND rename = nullptr;
    bool renaming = false;
    HWND empty = nullptr;
    std::array<HWND, 6> actions{};
    std::vector<Tile> tiles;
    std::vector<Tile> folderTiles;
    std::vector<GalleryClip> clips;
    std::vector<GalleryFolder> folders;
    int tileWidth = 0;
    int folderHeight = 0;
    int slide = 0;
    std::optional<bool> pendingSlide;
    int hoveredCrumb = -1;
    std::vector<RECT> crumbRects;

    // Height of the scrolling folder band above the clip grid.
    int headerHeight() const {
        if (folders.empty() || grid.columns < 1 || folderHeight <= 0) return 0;
        return static_cast<int>((folders.size() + grid.columns - 1) / grid.columns) * folderHeight;
    }
    // Keyed by path so a rescan that shifts indexes keeps decoded thumbnails.
    std::unordered_map<std::wstring, Thumbnail> cache;
    int decodeWidth = 0;
    std::filesystem::path directory;
    std::filesystem::path selectionPath;
    // Open folder chain below the clip directory; empty shows the library root.
    std::vector<std::filesystem::path> stack;

    std::filesystem::path currentFolder() const { return stack.empty() ? directory : stack.back(); }
    StudioGalleryLayout grid;
    std::optional<Viewport> viewport;
    size_t selected = noClip;
    int scroll = 0;
    int maxScroll = 0;
    int wheel = 0;
    POINT dragStart{};
    HWND dragTile = nullptr;
    bool scanning = false;
    bool manualRefresh = false;
    bool editing = false;
    bool dragActive = false;
    bool noticePending = false;

    std::mutex mutex;
    std::condition_variable_any cv;
    std::atomic<uint64_t> revision{0};
    uint64_t previewRevision = 0;
    size_t first = 0;
    size_t last = 0;
    std::optional<Scan> scan;
    std::optional<Snapshot> snapshot;
    std::optional<Edit> edit;
    std::optional<Edited> edited;
    std::deque<Request> requests;
    std::optional<PreviewKey> decoding;
    std::deque<Preview> previews;
    std::jthread worker;
};

void StudioWindow::Impl::buildLibrary() {
    if (!gallery) gallery = new Gallery(window);
    auto& g = *gallery;
    g.actions[0] = button(L"Open", [this] { openClip(); });
    g.actions[1] = button(L"Rename", [this] { beginRename(); });
    g.actions[2] = button(L"Favorite", [this] { editClip(ClipEdit::Favorite); });
    g.actions[3] = button(L"Refresh", [this] { refreshLibrary(true); });
    g.actions[4] = button(L"Delete", [this] { editClip(ClipEdit::Remove); });
    g.actions[5] = button(L"Back", [this] { backFolder(); });
    g.breadcrumb = make(L"STATIC", L"", SS_CENTERIMAGE | SS_NOTIFY | SS_NOPREFIX | SS_ENDELLIPSIS, {}, L"", body);
    if (g.breadcrumb) {
        controls.at(g.breadcrumb).mutedText = true;
        SetWindowSubclass(g.breadcrumb, galleryProc, 2, reinterpret_cast<DWORD_PTR>(this));
    }
    g.view = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"Clip gallery",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL | WS_TABSTOP, 0, 0, 0, 0, body, nullptr, nullptr, nullptr);
    if (!g.view) return;
    // The field replaces the selected tile's name line, so it belongs to the scrolling viewport.
    g.rename = make(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, {}, L"Rename the selected clip without changing its extension.", g.view);
    if (g.rename) {
        controls.at(g.rename).editBorder = true;
        controls.at(g.rename).margin = 5;
        SetWindowSubclass(g.rename, galleryProc, 2, reinterpret_cast<DWORD_PTR>(this));
    }
    SetWindowSubclass(g.view, galleryProc, 2, reinterpret_cast<DWORD_PTR>(this));
    SetWindowTheme(g.view, L"DarkMode_Explorer", nullptr);
    g.empty = make(L"STATIC", L"", SS_CENTER | SS_NOPREFIX, {}, L"", g.view);
    if (g.empty) controls.at(g.empty).mutedText = true;
    if (g.rename) {
        SendMessageW(g.rename, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(libraryPx(5), libraryPx(5)));
        SendMessageW(g.rename, EM_LIMITTEXT, 200, 0);
    }
    if (g.directory != saved.clipDirectory) {
        g.directory = saved.clipDirectory;
        g.clips.clear();
        g.selected = noClip;
        g.scroll = 0;
    }
    // The Library tab always lands on the root, not on the last open folder.
    g.stack.clear();
    refreshLibrary();
    g.selectionPath.clear();
    selectClip(g.selected);
}

void StudioWindow::Impl::closeLibrary() {
    if (!gallery || !gallery->view) return;
    auto& g = *gallery;
    {
        std::scoped_lock lock(g.mutex);
        ++g.revision;
        g.scan.reset();
        g.snapshot.reset();
    }
    g.resetPreviews();
    g.cancelDrag();
    g.cache.clear();
    g.folders.clear();
    g.tiles.clear();
    g.folderTiles.clear();
    g.viewport.reset();
    g.slide = 0;
    g.pendingSlide.reset();
    g.hoveredCrumb = -1;
    g.crumbRects.clear();
    motions[librarySlide] = {0.f, 0.f};
    g.view = g.breadcrumb = g.empty = g.rename = nullptr;
    g.actions.fill(nullptr);
    g.wheel = 0;
    g.scanning = false;
    g.manualRefresh = false;
    g.renaming = false;
}

void StudioWindow::Impl::requestLibraryStop() {
    if (gallery) gallery->worker.request_stop();
}

bool StudioWindow::Impl::libraryWorkerStopped() {
    // Poll the thread itself so COM teardown is also finished before joining.
    return !gallery || WaitForSingleObject(gallery->worker.native_handle(), 0) == WAIT_OBJECT_0;
}

void StudioWindow::Impl::destroyLibrary() {
    closeLibrary();
    delete std::exchange(gallery, nullptr);
}

void StudioWindow::Impl::refreshLibrary(bool userInitiated) {
    if (closing || shuttingDown || nav.page != Page::Library || !gallery || !gallery->view) return;
    auto& g = *gallery;
    g.scanning = true;
    g.manualRefresh = userInitiated;
    {
        std::scoped_lock lock(g.mutex);
        g.scan = Gallery::Scan{++g.revision, g.currentFolder(), saved.libraryShowHiddenFolders};
    }
    g.cv.notify_one();
    if (userInitiated) {
        if (g.actions[3]) {
            setText(g.actions[3], L"Refreshing\u2026");
            EnableWindow(g.actions[3], FALSE);
            InvalidateRect(g.actions[3], nullptr, FALSE);
        }
        setStatus(L"Refreshing library\u2026");
    }
    if (g.clips.empty() && g.folders.empty()) setText(g.empty, L"Loading clips\u2026");
}

void StudioWindow::Impl::galleryChanged() {
    if (!gallery) return;
    auto& g = *gallery;
    std::optional<Gallery::Snapshot> snapshot;
    std::optional<Gallery::Edited> edit;
    std::deque<Gallery::Preview> previews;
    {
        std::scoped_lock lock(g.mutex);
        snapshot = std::exchange(g.snapshot, std::nullopt);
        edit = std::exchange(g.edited, std::nullopt);
        previews.swap(g.previews);
        g.noticePending = false;
    }
    const auto invalidateTile = [&](Gallery::Tile& tile) {
        if (g.dragTile == tile.hwnd) g.cancelDrag();
        if (GetFocus() == tile.hwnd) SetFocus(g.view);
        tile.index = noClip;
    };
    bool layoutChanged = false;
    bool refocus = false;
    if (edit) {
        g.editing = false;
        if (!edit->ok) setStatus(L"Clip could not be changed. Check that it still exists and is not in use.");
        else if (g.directory == edit->edit.directory) {
            const auto& change = edit->edit;
            const auto it = std::ranges::find(g.clips, change.path, [](const auto& clip) -> const auto& { return clip.info.path; });
            const auto index = static_cast<size_t>(it - g.clips.begin());
            switch (change.kind) {
            case ClipEdit::Favorite:
                if (it != g.clips.end()) {
                    it->info.favorite = change.favorite;
                    if (const auto tile = g.tileFor(index)) InvalidateRect(tile, nullptr, FALSE);
                }
                break;
            case ClipEdit::Remove:
                if (it != g.clips.end()) {
                    for (auto& tile : g.tiles) if (tile.index != noClip && tile.index >= index) invalidateTile(tile);
                    g.clips.erase(it);
                    g.cache.erase(change.path.native());
                    if (index == g.selected) {
                        g.selected = g.clips.empty() ? noClip : (std::min)(index, g.clips.size() - 1);
                        g.selectionPath = g.selected == noClip ? std::filesystem::path{} : g.clips[g.selected].info.path;
                        const auto focus = GetFocus();
                        refocus = !focus || focus == g.view || GetParent(focus) == g.view || focus == g.actions[4];
                    } else if (g.selected != noClip && index < g.selected) --g.selected;
                    layoutChanged = true;
                }
                break;
            case ClipEdit::Rename:
                // The move keeps the modified time, so the list order holds without a rescan.
                if (it != g.clips.end()) {
                    it->info.path = clipRenameTarget(change.path, change.name);
                    it->name = it->info.path.filename().wstring();
                    if (const auto tile = g.tileFor(index)) {
                        if (g.dragTile == tile) g.cancelDrag();
                        setText(tile, it->name);
                    }
                    if (g.selectionPath == change.path) g.selectionPath = it->info.path;
                    if (auto node = g.cache.extract(change.path.native())) {
                        node.key() = it->info.path.native();
                        g.cache.insert(std::move(node));
                    }
                    if (index == g.selected) {
                        const auto focus = GetFocus();
                        refocus = focus == g.view;
                    }
                    layoutChanged = true;
                }
                break;
            }
            setStatus(change.kind == ClipEdit::Remove ? L"Clip moved to the Recycle Bin" : change.kind == ClipEdit::Rename ? L"Clip renamed" :
                      change.favorite ? L"Clip favorited" : L"Favorite removed");
        }
    }
    if (!g.view) return;
    const bool scanFinished = snapshot && snapshot->revision == g.revision.load();
    if (scanFinished) {
        g.scanning = false;
        const bool wasManual = std::exchange(g.manualRefresh, false);
        if (snapshot->ok) {
            for (auto& tile : g.tiles) {
                const auto i = tile.index;
                if (i == noClip) continue;
                if (i >= snapshot->clips.size() || g.clips[i].info.path != snapshot->clips[i].info.path) invalidateTile(tile);
                else if (g.dragTile == tile.hwnd && !sameMedia(g.clips[i].info, snapshot->clips[i].info)) g.cancelDrag();
            }
            std::erase_if(g.cache, [](const auto& entry) { return !entry.second.bitmap.source; });
            g.clips = std::move(snapshot->clips);
            g.folders = std::move(snapshot->folders);
            if (g.selected >= g.clips.size() || g.clips[g.selected].info.path != g.selectionPath) {
                g.selected = noClip;
                if (!g.selectionPath.empty()) {
                    const auto it = std::ranges::find(g.clips, g.selectionPath, [](const auto& clip) -> const auto& { return clip.info.path; });
                    if (it != g.clips.end()) g.selected = static_cast<size_t>(it - g.clips.begin());
                }
            }
            layoutChanged = true;
            if (wasManual) setStatus(L"Library refreshed.");
        } else {
            if (g.clips.empty() && g.folders.empty()) setText(g.empty, L"Clip library could not be read.");
            setStatus(L"Clip library could not be read.");
        }
    }
    if (layoutChanged) {
        g.viewport.reset();
        g.resetPreviews();
        previews.clear();
    }
    for (auto& preview : previews) {
        const auto index = preview.request.index;
        const auto& info = preview.request.info;
        if (index >= g.clips.size() || !sameMedia(g.clips[index].info, info) ||
            preview.request.revision != g.previewRevision || index < g.first || index >= g.last) continue;
        auto& entry = g.cache[info.path.native()];
        // A failed decode keeps an older bitmap and still records the width so it is not requeued.
        const bool replace = preview.bitmap.source || !entry.bitmap.source || entry.bytes != info.bytes ||
                             entry.modifiedTime != info.modifiedTime;
        if (replace) entry.bitmap = std::move(preview.bitmap);
        entry.bytes = info.bytes;
        entry.modifiedTime = info.modifiedTime;
        entry.width = preview.request.width;
        if (replace && entry.bitmap.source) {
            if (const auto tile = g.tileFor(index)) InvalidateRect(tile, nullptr, FALSE);
        }
    }
    if (edit || scanFinished) selectClip(g.selected);
    if (layoutChanged) updateGallery();
    if (scanFinished && snapshot->ok && g.pendingSlide)
        startGallerySlide(*std::exchange(g.pendingSlide, std::nullopt));
    if (refocus) selectClip(g.selected, true);
    if (edit || snapshot) g.cv.notify_one();
}

void StudioWindow::Impl::openFolder(size_t index) {
    if (closing || shuttingDown || !gallery || !gallery->view || index >= gallery->folders.size()) return;
    auto next = gallery->stack;
    next.push_back(gallery->folders[index].path);
    showLibraryFolder(std::move(next), true);
}

void StudioWindow::Impl::backFolder() {
    if (closing || shuttingDown || !gallery || !gallery->view || gallery->stack.empty()) return;
    auto next = gallery->stack;
    next.pop_back();
    showLibraryFolder(std::move(next), false);
}

bool StudioWindow::Impl::libraryToRoot() {
    if (!gallery || !gallery->view || gallery->stack.empty()) return false;
    showLibraryFolder({}, false);
    return true;
}

void StudioWindow::Impl::showLibraryFolder(std::vector<std::filesystem::path> stack, bool forward) {
    auto& g = *gallery;
    g.stack = std::move(stack);
    g.clips.clear();
    g.folders.clear();
    g.scroll = 0;
    g.viewport.reset();
    g.hoveredCrumb = -1;
    g.crumbRects.clear();
    motions[librarySlide] = {0.f, 0.f};
    g.slide = 0;
    if (g.breadcrumb) {
        std::wstring crumb;
        if (!g.stack.empty()) {
            crumb = L"Library";
            for (const auto& folder : g.stack) {
                crumb += L"  \u203a  ";
                crumb += folder.filename().wstring();
            }
        }
        setText(g.breadcrumb, crumb);
        InvalidateRect(g.breadcrumb, nullptr, FALSE);
    }
    selectClip(noClip);
    g.pendingSlide = forward;
    refreshLibrary();
    updateGallery();
}

void StudioWindow::Impl::startGallerySlide(bool forward) {
    auto& motion = motions[librarySlide];
    RECT client{};
    const int distance = gallery && gallery->view && GetClientRect(gallery->view, &client) ? client.right : 0;
    const bool animate = motionEnabled && !closing && !sizing && IsWindowEnabled(window) &&
        IsWindowVisible(window) && !IsIconic(window) && distance > 0;
    if (!animate) {
        motion = {0.f, 0.f};
    } else {
        motion = {static_cast<float>(forward ? distance : -distance), 0.f, SteadyClock::now(), std::chrono::milliseconds{200}};
        if (!SetTimer(window, motionTimer, 16, nullptr)) motion = {0.f, 0.f};
    }
    invalidateMotion(librarySlide);
}

int StudioWindow::Impl::gallerySlide() const {
    return static_cast<int>(std::lround(motionValue(librarySlide)));
}


bool StudioWindow::Impl::libraryInput(UINT msg, WPARAM wp) {
    if (nav.page != Page::Library || !gallery || !gallery->view) return false;
    auto& g = *gallery;
    if (msg == WM_XBUTTONUP) {
        if (GET_XBUTTON_WPARAM(wp) != XBUTTON1) return false;
        backFolder();
        return true;
    }
    if (msg != WM_MOUSEWHEEL) return false;
    g.wheel += GET_WHEEL_DELTA_WPARAM(wp);
    const auto steps = g.wheel / WHEEL_DELTA;
    g.wheel %= WHEEL_DELTA;
    if (steps) {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const int64_t distance = lines == WHEEL_PAGESCROLL ? (g.viewport ? g.viewport->height : 0) :
            libraryPx(25) * static_cast<int>((std::min)(lines, 100U));
        if (g.scrollTo(g.scroll - steps * distance)) updateGallery();
    }
    return true;
}

void StudioWindow::Impl::layoutGallery() {
    if (!gallery || !gallery->view) return;
    auto& g = *gallery;
    RECT r{};
    GetClientRect(body, &r);
    const int pad = libraryPx(theme::inset), gap = libraryPx(theme::gap);
    const int width = (std::max)(0L, r.right - pad * 2);
    const int row = libraryPx(30), y = libraryPx(9);
    // Shrink toolbar buttons evenly when narrow or zoomed so left and right
    // groups never overlap.
    const int toolbarGap = (std::min)(gap, (std::max)(0, width / 12));
    const int buttonWidth = (std::min)(libraryPx(92), (std::max)(1, (width - 5 * toolbarGap) / 6));
    std::vector<WindowMove> moves;
    for (int i = 0; i < 3; ++i) moves.push_back({g.actions[i], pad + i * (buttonWidth + toolbarGap), y, buttonWidth, row});
    moves.push_back({g.actions[5], pad + width - 3 * buttonWidth - 2 * toolbarGap, y, buttonWidth, row});
    moves.push_back({g.actions[3], pad + width - 2 * buttonWidth - toolbarGap, y, buttonWidth, row});
    moves.push_back({g.actions[4], pad + width - buttonWidth, y, buttonWidth, row});
    int top = y + row + gap * 2;
    if (g.breadcrumb) {
        const bool showCrumb = !g.stack.empty();
        if (showCrumb) {
            const int crumbHeight = libraryPx(22);
            moves.push_back({g.breadcrumb, pad, y + row + gap, width, crumbHeight});
            top = y + row + gap + crumbHeight + gap;
        }
        if (((GetWindowLongPtrW(g.breadcrumb, GWL_STYLE) & WS_VISIBLE) != 0) != showCrumb)
            ShowWindow(g.breadcrumb, showCrumb ? SW_SHOWNA : SW_HIDE);
    }
    const int slide = gallerySlide();
    moves.push_back({g.view, slide, top, r.right, (std::max)(0L, r.bottom - top)});
    applyWindowMoves(moves);
    if (g.slide != slide) {
        // The strip the viewport stopped covering belongs to the surface again.
        const int lo = (std::min)(g.slide, slide), hi = (std::max)(g.slide, slide);
        if (lo < 0) {
            RECT strip{r.right + lo, top, r.right, r.bottom};
            InvalidateRect(body, &strip, FALSE);
        }
        if (hi > 0) {
            RECT strip{0, top, hi, r.bottom};
            InvalidateRect(body, &strip, FALSE);
        }
        g.slide = slide;
        redrawWindowAndChildren(g.view);
    }
    updateGallery();
}

void StudioWindow::Impl::updateGallery() {
    if (!gallery || !gallery->view || shuttingDown) return;
    auto& g = *gallery;
    RECT r{};
    GetClientRect(g.view, &r);
    const int pad = libraryPx(theme::inset), gap = libraryPx(theme::gap);
    int width = r.right, height = r.bottom;
    int columns = 1, tileWidth = 1, rowHeight = 1, header = 0, bottom = 0;
    const auto computeLayout = [&](int viewWidth) {
        const int fitted = std::clamp((viewWidth + GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) - pad * 2 + gap) / libraryPx(210), 1, 5);
        // The gap arithmetic is int, so an unusable count still has to stay below it.
        columns = saved.libraryColumns ? static_cast<int>((std::min)(saved.libraryColumns, 10000u)) : fitted;
        tileWidth = (std::max)(1, (viewWidth - pad * 2 - (columns - 1) * gap) / columns);
        // Fitting fills the tile: the row takes the frame's 16:9 shape, so the
        // thumbnail has no bars and nothing is cropped away. Unfitted keeps one tile
        // height and scales the frame into it, so a wide tile shows bars.
        const int naturalHeight = tileWidth * 9 / 16 + libraryPx(59);
        rowHeight = saved.libraryFitThumbnails
            ? (std::min)(naturalHeight, (std::max)(height, libraryPx(250)))
            : (std::min)(libraryPx(250), naturalHeight);
        int64_t position = g.scroll;
        if (g.grid.columns != columns || g.grid.rowHeight != rowHeight)
            position = position / g.grid.rowHeight * g.grid.columns / columns * rowHeight;
        g.grid = {columns, rowHeight};
        g.tileWidth = tileWidth;
        g.folderHeight = libraryPx(34);
        header = g.headerHeight();
        const auto rowCount = g.grid.rowCount(g.clips.size());
        bottom = header + static_cast<int>((std::min)(rowCount, static_cast<size_t>(INT_MAX / rowHeight))) * rowHeight;
        g.maxScroll = (std::max)(0, bottom - height);
        g.scrollTo(position);
    };
    computeLayout(width);
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0,
                    (std::max)(0, bottom - 1), static_cast<UINT>(r.bottom), g.scroll};
    SetScrollInfo(g.view, SB_VERT, &info, FALSE);
    // The scrollbar can appear or disappear, changing the usable client width.
    GetClientRect(g.view, &r);
    if (r.right != width) {
        width = r.right;
        height = r.bottom;
        computeLayout(width);
        info.nMax = (std::max)(0, bottom - 1);
        info.nPage = static_cast<UINT>(r.bottom);
        info.nPos = g.scroll;
        SetScrollInfo(g.view, SB_VERT, &info, FALSE);
    }
    const bool placeRename = g.renaming && g.rename && g.selected < g.clips.size();
    if (placeRename) {
        const int nameTop = rowHeight - libraryPx(4) - libraryPx(42);
        SetWindowPos(g.rename, HWND_TOP,
            pad + static_cast<int>(g.selected % g.grid.columns) * (tileWidth + gap) + libraryPx(2),
            header + static_cast<int>(g.selected / g.grid.columns) * rowHeight - g.scroll + nameTop,
            (std::max)(libraryPx(30), tileWidth - libraryPx(4)), libraryPx(21), SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
    }
    const Gallery::Viewport viewport{width, height, g.scroll, dpi};
    if (g.viewport == viewport) {
        if (placeRename) redrawWindowAndChildren(g.view);
        return;
    }
    g.viewport = viewport;
    if (g.clips.empty() && g.folders.empty()) {
        const std::wstring message = g.scanning ? L"Loading clips\u2026"
            : !g.stack.empty() ? L"No clips in this folder."
            : L"No clips yet.\r\nClips are saved to " + g.directory.wstring();
        setText(g.empty, message);
        moveWindowIfChanged(g.empty, pad, libraryPx(20), (std::max)(0, static_cast<int>(r.right) - pad * 2), libraryPx(64));
        ShowWindow(g.empty, SW_SHOWNA);
    } else ShowWindow(g.empty, SW_HIDE);
    const int headerShown = std::clamp(header - g.scroll, 0, height);
    const int clipScroll = (std::max)(0, g.scroll - header);
    const int clipHeight = (std::max)(0, height - headerShown);
    const auto [first, drawnEnd] = g.grid.visible(g.clips.size(), clipScroll, clipHeight);
    const auto focus = GetFocus();
    size_t focusClip = noClip;
    for (const auto& tile : g.tiles) if (tile.hwnd == focus) focusClip = tile.index;
    if (focusClip != noClip && (focusClip < first || focusClip >= drawnEnd)) SetFocus(g.view);
    // A rotating pool keeps a visible tile's HWND and focus when only the first row moves.
    const size_t used = drawnEnd - first;
    if (used > g.tiles.size()) {
        if (focusClip != noClip) SetFocus(g.view);
        while (g.tiles.size() < used) {
            const auto slot = g.tiles.size();
            const auto tile = button(L"", [this, slot] {
                if (gallery && slot < gallery->tiles.size()) selectClip(gallery->tiles[slot].index);
            }, L"", g.view);
            if (!tile) break;
            SetWindowLongPtrW(tile, GWLP_USERDATA, slot + 1);
            SetWindowSubclass(tile, galleryProc, 2, reinterpret_cast<DWORD_PTR>(this));
            g.tiles.push_back({tile});
        }
    }
    std::vector<WindowMove> moves;
    // Folder rows are native buttons like the clips, so a click is an ordinary
    // button click instead of a hit test against hand-painted rects.
    if (g.folders.size() > g.folderTiles.size()) {
        while (g.folderTiles.size() < g.folders.size()) {
            const auto slot = g.folderTiles.size();
            const auto tile = button(L"", [this, slot] {
                if (gallery && slot < gallery->folderTiles.size()) openFolder(gallery->folderTiles[slot].index);
            }, L"", g.view);
            if (!tile) break;
            SetWindowLongPtrW(tile, GWLP_USERDATA, slot + 1);
            SetWindowSubclass(tile, galleryProc, 2, reinterpret_cast<DWORD_PTR>(this));
            g.folderTiles.push_back({tile});
        }
    }
    const int folderCell = (std::max)(1, g.folderHeight - gap);
    for (size_t slot = 0; slot < g.folderTiles.size(); ++slot) {
        auto& tile = g.folderTiles[slot];
        if (slot >= g.folders.size()) {
            tile.index = noClip;
            if (GetWindowLongPtrW(tile.hwnd, GWL_STYLE) & WS_VISIBLE) ShowWindow(tile.hwnd, SW_HIDE);
            continue;
        }
        if (tile.index != slot) {
            tile.index = slot;
            setText(tile.hwnd, g.folders[slot].name);
        }
        moves.push_back({tile.hwnd, pad + static_cast<int>(slot % columns) * (tileWidth + gap),
            static_cast<int>(slot / columns) * g.folderHeight - g.scroll, tileWidth, folderCell});
    }
    const auto count = g.tiles.size();
    moves.reserve(count);
    for (size_t slot = 0; slot < count; ++slot) {
        const auto index = first + (slot + count - first % count) % count;
        auto& tile = g.tiles[slot];
        if (index >= drawnEnd || tile.index != index) {
            if (g.dragTile == tile.hwnd) g.cancelDrag();
            tile.index = index < drawnEnd ? index : noClip;
            if (tile.index != noClip) setText(tile.hwnd, g.clips[index].name);
        }
        if (tile.index == noClip) {
            if (GetWindowLongPtrW(tile.hwnd, GWL_STYLE) & WS_VISIBLE) ShowWindow(tile.hwnd, SW_HIDE);
            continue;
        }
        moves.push_back({tile.hwnd, pad + static_cast<int>(index % columns) * (tileWidth + gap),
            header + static_cast<int>(index / columns) * rowHeight - g.scroll, tileWidth, rowHeight});
    }
    applyWindowMoves(moves);
    for (const auto& tile : g.tiles) if (tile.index != noClip && !(GetWindowLongPtrW(tile.hwnd, GWL_STYLE) & WS_VISIBLE))
        ShowWindow(tile.hwnd, SW_SHOWNA);
    for (const auto& tile : g.folderTiles) if (tile.index != noClip && !(GetWindowLongPtrW(tile.hwnd, GWL_STYLE) & WS_VISIBLE))
        ShowWindow(tile.hwnd, SW_SHOWNA);
    if (GetFocus() == g.view) {
        if (const auto tile = g.tileFor(focusClip)) SetFocus(tile);
    }
    redrawWindowAndChildren(g.view);
    if (closing) return;
    const auto decodeWidth = thumbnailWidth(tileWidth);
    // A larger decode supersedes queued and in-flight smaller ones.
    if (decodeWidth > g.decodeWidth) g.resetPreviews();
    g.decodeWidth = decodeWidth;
    const auto last = (std::min)(g.grid.visible(g.clips.size(), clipScroll, clipHeight, 1).second, first + thumbnailBudget);
    if (first == g.first && last == g.last) return;
    {
        std::unordered_map<std::wstring, Thumbnail> kept;
        for (auto i = first; i < last; ++i) {
            const auto& info = g.clips[i].info;
            if (auto node = g.cache.extract(info.path.native());
                node && node.mapped().bytes == info.bytes && node.mapped().modifiedTime == info.modifiedTime)
                kept.insert(std::move(node));
        }
        g.cache.swap(kept);
    }
    {
        std::scoped_lock lock(g.mutex);
        g.first = first;
        g.last = last;
        const auto inRange = [&](size_t index) { return index >= first && index < last; };
        std::erase_if(g.previews, [&](const auto& preview) { return !inRange(preview.request.index); });
        std::array<bool, thumbnailBudget> pending{};
        for (const auto& preview : g.previews) pending[preview.request.index - first] = true;
        if (g.decoding && g.decoding->first == g.previewRevision && inRange(g.decoding->second))
            pending[g.decoding->second - first] = true;
        g.requests.clear();
        for (auto i = first; i < last; ++i) {
            const auto cached = g.cache.find(g.clips[i].info.path.native());
            if (!pending[i - first] && (cached == g.cache.end() || cached->second.width < decodeWidth))
                g.requests.push_back({g.previewRevision, i, g.clips[i].info, decodeWidth});
        }
    }
    g.cv.notify_one();
}

void StudioWindow::Impl::selectClip(size_t index, bool focus) {
    if (!gallery || !gallery->view) return;
    auto& g = *gallery;
    const auto old = g.selected;
    const bool selected = index < g.clips.size();
    g.selected = selected ? index : noClip;
    if (g.renaming && old != g.selected) endRename();
    for (const auto i : {0, 1, 2, 4}) EnableWindow(g.actions[i], selected && !g.editing);
    const bool refreshing = g.scanning && g.manualRefresh;
    setText(g.actions[3], refreshing ? L"Refreshing\u2026" : L"Refresh");
    EnableWindow(g.actions[3], !g.editing && !refreshing);
    EnableWindow(g.actions[5], !g.editing);
    // Back only exists inside a folder; its slot stays reserved so the other
    // actions do not move.
    ShowWindow(g.actions[5], g.stack.empty() ? SW_HIDE : SW_SHOWNA);
    setText(g.actions[2], selected && g.clips[index].info.favorite ? L"Unfavorite" : L"Favorite");
    if (selected) {
        const auto& path = g.clips[index].info.path;
        if (g.selectionPath != path) g.selectionPath = path;
    } else {
        g.selectionPath.clear();
    }
    if (focus && selected) {
        RECT r{};
        GetClientRect(g.view, &r);
        const auto y = static_cast<int64_t>(g.headerHeight()) +
                       static_cast<int64_t>(index / g.grid.columns) * g.grid.rowHeight;
        if (r.bottom > 0) {
            const auto position = y < g.scroll ? y : (std::max)(int64_t{g.scroll}, y + g.grid.rowHeight - r.bottom);
            if (g.scrollTo(position)) updateGallery();
        }
        if (const auto tile = g.tileFor(index)) SetFocus(tile);
    }
    if (old != g.selected) {
        for (auto changed : {old, g.selected}) if (const auto tile = g.tileFor(changed)) InvalidateRect(tile, nullptr, FALSE);
    }
}

void StudioWindow::Impl::focusFolder(size_t index) {
    if (!gallery || !gallery->view || index >= gallery->folders.size() || index >= gallery->folderTiles.size()) return;
    auto& g = *gallery;
    selectClip(noClip);
    RECT r{};
    GetClientRect(g.view, &r);
    const auto y = static_cast<int64_t>(index / g.grid.columns) * g.folderHeight;
    if (r.bottom > 0) {
        const auto position = y < g.scroll ? y : (std::max)(int64_t{g.scroll}, y + g.folderHeight - r.bottom);
        if (g.scrollTo(position)) updateGallery();
    }
    SetFocus(g.folderTiles[index].hwnd);
}

void StudioWindow::Impl::beginRename() {
    if (closing || shuttingDown || !gallery || !gallery->view || !gallery->rename || gallery->editing ||
        gallery->selected >= gallery->clips.size()) return;
    auto& g = *gallery;
    RECT r{};
    GetClientRect(g.view, &r);
    const int cols = (std::max)(1, g.grid.columns);
    const int y = g.headerHeight() + static_cast<int>(g.selected / cols) * g.grid.rowHeight;
    if (r.bottom > 0 && g.grid.rowHeight > 0) {
        if (g.grid.rowHeight <= r.bottom) {
            const int position = y < g.scroll ? y : (std::max)(g.scroll, y + g.grid.rowHeight - static_cast<int>(r.bottom));
            g.scrollTo(position);
        } else {
            const int nameTop = g.grid.rowHeight - libraryPx(4) - libraryPx(42);
            const int nameBottom = nameTop + libraryPx(21);
            const int position = (y + nameTop < g.scroll) ? (y + nameTop)
                : (std::max)(g.scroll, y + nameBottom - static_cast<int>(r.bottom));
            g.scrollTo(position);
        }
    }
    g.renaming = true;
    setText(g.rename, g.clips[g.selected].info.path.stem().wstring());
    ShowWindow(g.rename, SW_SHOWNA);
    updateGallery();
    SetFocus(g.rename);
    SendMessageW(g.rename, EM_SETSEL, 0, -1);
}

void StudioWindow::Impl::endRename() {
    if (!gallery || !gallery->view || !gallery->rename || !gallery->renaming) return;
    auto& g = *gallery;
    const bool hadFocus = GetFocus() == g.rename;
    g.renaming = false;
    ShowWindow(g.rename, SW_HIDE);
    if (hadFocus) {
        if (const auto tile = g.tileFor(g.selected)) SetFocus(tile);
        else SetFocus(g.view);
    }
}

void StudioWindow::Impl::openClip() {
    if (!gallery || !gallery->view || gallery->editing || gallery->selected >= gallery->clips.size()) return;
    if (!shellExecuteSucceeded(ShellExecuteW(window, L"open", gallery->selectionPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL)))
        setStatus(L"Clip could not be opened.");
}

void StudioWindow::Impl::editClip(ClipEdit kind) {
    if (closing || shuttingDown || !gallery || !gallery->view || gallery->editing || gallery->selected >= gallery->clips.size()) return;
    auto& g = *gallery;
    const auto& info = g.clips[g.selected].info;
    Gallery::Edit change{kind, g.directory, info.path, {}, !info.favorite};
    if (kind == ClipEdit::Rename) {
        // Validate here so a rejected name keeps the field open with the typed text.
        const auto typed = readText(g.rename);
        const auto target = clipRenameTarget(info.path, typed);
        if (target.empty()) {
            setStatus(trimText(typed).empty() ? L"Enter a clip name." : L"Name contains characters Windows does not allow.");
            return;
        }
        if (target.native() == info.path.native()) { endRename(); return; }
        if (clipRenameCollides(info.path, target)) { setStatus(L"A clip with that name already exists."); return; }
        change.name = target.stem().wstring();
        endRename();
    } else if (kind == ClipEdit::Remove) {
        const auto view = g.view;
        const auto target = info;
        if (MessageBoxW(window, (L"Move " + target.path.filename().wstring() + L" to the Recycle Bin?").c_str(), L"Delete clip",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) return;
        if (closing || shuttingDown || !gallery || gallery->view != view || gallery->directory != change.directory || gallery->editing)
            return;
        if (g.selected >= g.clips.size() || !sameMedia(target, g.clips[g.selected].info)) {
            setStatus(L"Clip changed. Select it again to delete it.");
            return;
        }
    }
    g.editing = true;
    {
        std::scoped_lock lock(g.mutex);
        assert(!g.edit);
        g.edit = std::move(change);
        // A scan that began before the edit must not publish pre-edit state.
        if (g.scanning && !g.scan) g.scan = Gallery::Scan{++g.revision, g.currentFolder()};
    }
    g.cv.notify_one();
    for (const auto i : {0, 1, 2, 4}) EnableWindow(g.actions[i], FALSE);
}

bool StudioWindow::Impl::galleryShortcut(const MSG& msg) {
    if (!gallery || !gallery->view) return false;
    auto& g = *gallery;
    const auto focus = GetFocus();
    const bool tile = focus && GetParent(focus) == g.view;
    const bool inGallery = focus == g.view || tile;
    // The rename field keeps its keys: Delete and the refresh shortcut belong to the typed text.
    if (focus == g.rename) {
        if (msg.wParam == VK_ESCAPE) endRename();
        else if (msg.wParam == VK_RETURN) editClip(ClipEdit::Rename);
        else return false;
        return true;
    }
    if (msg.wParam == VK_F5 || (isControlDown() && msg.wParam == L'R')) { refreshLibrary(true); return true; }
    if (msg.wParam == VK_LEFT && (GetKeyState(VK_MENU) & 0x8000) != 0) {
        if (!g.stack.empty()) backFolder();
        return true;
    }
    if (msg.wParam == VK_BACK && !g.stack.empty()) { backFolder(); return true; }
    size_t focusedFolder = noClip;
    if (tile) {
        for (const auto& ft : g.folderTiles) {
            if (ft.hwnd == focus && ft.index < g.folders.size()) {
                focusedFolder = ft.index;
                break;
            }
        }
    }
    if (isControlDown() && msg.wParam == L'O') { openClip(); return true; }
    if (msg.message == WM_KEYDOWN && !isControlDown() && !(GetKeyState(VK_MENU) & 0x8000) &&
        msg.wParam == L'F' && tile && focusedFolder == noClip && g.selected < g.clips.size()) {
        editClip(ClipEdit::Favorite);
        return true;
    }
    if (inGallery && msg.wParam == VK_RETURN) {
        if (focusedFolder != noClip) openFolder(focusedFolder);
        else openClip();
        return true;
    }
    if (msg.wParam == VK_DELETE) { editClip(ClipEdit::Remove); return true; }
    if (msg.wParam == VK_F2 && g.selected < g.clips.size()) { beginRename(); return true; }
    if (!inGallery || (g.clips.empty() && g.folders.empty())) return false;
    const auto cols = static_cast<ptrdiff_t>((std::max)(1, g.grid.columns));
    const auto folderCount = static_cast<ptrdiff_t>(g.folders.size());
    const auto clipCount = static_cast<ptrdiff_t>(g.clips.size());
    if (focusedFolder != noClip) {
        const auto fi = static_cast<ptrdiff_t>(focusedFolder);
        switch (msg.wParam) {
        case VK_LEFT:
            if (fi > 0) focusFolder(static_cast<size_t>(fi - 1));
            return true;
        case VK_RIGHT:
            if (fi + 1 < folderCount) focusFolder(static_cast<size_t>(fi + 1));
            else if (clipCount > 0) selectClip(0, true);
            return true;
        case VK_UP:
            if (fi >= cols) focusFolder(static_cast<size_t>(fi - cols));
            return true;
        case VK_DOWN:
            if (fi + cols < folderCount) focusFolder(static_cast<size_t>(fi + cols));
            else if (clipCount > 0) selectClip(static_cast<size_t>((std::min)(fi % cols, clipCount - 1)), true);
            return true;
        case VK_HOME:
        case VK_PRIOR:
            focusFolder(0);
            return true;
        case VK_END:
            if (clipCount > 0) selectClip(static_cast<size_t>(clipCount - 1), true);
            else focusFolder(static_cast<size_t>(folderCount - 1));
            return true;
        case VK_NEXT:
            if (clipCount > 0) selectClip(0, true);
            else focusFolder(static_cast<size_t>(folderCount - 1));
            return true;
        default: return false;
        }
    }
    if (clipCount == 0) {
        switch (msg.wParam) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_PRIOR:
            focusFolder(0);
            return true;
        case VK_END: case VK_NEXT:
            focusFolder(static_cast<size_t>(folderCount - 1));
            return true;
        default: return false;
        }
    }
    const auto current = static_cast<ptrdiff_t>(g.selected == noClip ? 0 : g.selected);
    const auto last = clipCount - 1;
    if (g.selected == noClip && folderCount > 0 &&
        (msg.wParam == VK_HOME || msg.wParam == VK_LEFT || msg.wParam == VK_UP)) {
        focusFolder(0);
        return true;
    }
    if (g.selected != noClip && folderCount > 0) {
        if (msg.wParam == VK_HOME) { focusFolder(0); return true; }
        if (msg.wParam == VK_LEFT && current == 0) { focusFolder(static_cast<size_t>(folderCount - 1)); return true; }
        if (msg.wParam == VK_UP && current < cols) {
            const ptrdiff_t lastFolderRow = (folderCount - 1) / cols;
            const ptrdiff_t target = (std::min)(lastFolderRow * cols + (current % cols), folderCount - 1);
            focusFolder(static_cast<size_t>(target));
            return true;
        }
    }
    ptrdiff_t next = current;
    switch (msg.wParam) {
    case VK_LEFT: --next; break;
    case VK_RIGHT: ++next; break;
    case VK_UP: next -= cols; break;
    case VK_DOWN: next += cols; break;
    case VK_HOME: next = 0; break;
    case VK_END: next = last; break;
    case VK_PRIOR: case VK_NEXT: {
        RECT r{};
        GetClientRect(g.view, &r);
        next += (msg.wParam == VK_PRIOR ? -1 : 1) * cols * (std::max)(1L, r.bottom / g.grid.rowHeight - 1);
        break;
    }
    default: return false;
    }
    selectClip(static_cast<size_t>(g.selected == noClip && msg.wParam != VK_END ? 0 : std::clamp(next, ptrdiff_t{}, last)), true);
    return true;
}

LRESULT CALLBACK StudioWindow::Impl::galleryProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto& self = *reinterpret_cast<Impl*>(data);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, galleryProc, id);
    if (!self.gallery || !self.gallery->view) return DefSubclassProc(hwnd, msg, wp, lp);
    auto& g = *self.gallery;
    // Wheel and the side mouse buttons belong to the page, not to the child that
    // happens to sit under the cursor.
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN) {
        self.setFocusCuesVisible(false);
    }
    if (self.libraryInput(msg, wp)) return 0;
    if (hwnd == g.breadcrumb) {
        switch (msg) {
        case WM_ERASEBKGND: return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(hwnd, &ps);
            RECT r{};
            GetClientRect(hwnd, &r);
            theme::fill(dc, r, theme::surface);
            g.crumbRects.clear();
            if (!g.stack.empty()) {
                SetBkMode(dc, TRANSPARENT);
                SelectObject(dc, self.pageFont());
                int x = r.left;
                const std::wstring sep = L"  \u203a  ";
                SIZE sepSize{};
                GetTextExtentPoint32W(dc, sep.c_str(), static_cast<int>(sep.size()), &sepSize);
                const size_t total = g.stack.size() + 1;
                for (size_t i = 0; i < total && x < r.right; ++i) {
                    const std::wstring segment = i == 0 ? L"Library" : g.stack[i - 1].filename().wstring();
                    SIZE size{};
                    GetTextExtentPoint32W(dc, segment.c_str(), static_cast<int>(segment.size()), &size);
                    RECT segRect{x, r.top, (std::min)(r.right, static_cast<LONG>(x + size.cx)), r.bottom};
                    const bool clickable = i + 1 < total;
                    const bool hot = clickable && g.hoveredCrumb == static_cast<int>(i);
                    SetTextColor(dc, clickable ? (hot ? theme::text : theme::muted) : theme::text);
                    DrawTextW(dc, segment.c_str(), -1, &segRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                    if (hot) {
                        const int underlineY = (r.top + r.bottom + size.cy) / 2;
                        theme::fill(dc, {segRect.left, underlineY, segRect.right, underlineY + (std::max)(1, self.libraryPx(1))}, theme::muted);
                    }
                    if (clickable) g.crumbRects.push_back(segRect);
                    x = segRect.right;
                    if (i + 1 < total && x < r.right) {
                        RECT sepRect{x, r.top, (std::min)(r.right, static_cast<LONG>(x + sepSize.cx)), r.bottom};
                        SetTextColor(dc, theme::disabled);
                        DrawTextW(dc, sep.c_str(), -1, &sepRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                        x = sepRect.right;
                    }
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hit = -1;
            for (size_t i = 0; i < g.crumbRects.size(); ++i) {
                if (PtInRect(&g.crumbRects[i], pt)) { hit = static_cast<int>(i); break; }
            }
            if (hit != g.hoveredCrumb) {
                g.hoveredCrumb = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g.hoveredCrumb != -1) {
                g.hoveredCrumb = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (g.hoveredCrumb >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP: {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            for (size_t i = 0; i < g.crumbRects.size(); ++i) {
                if (PtInRect(&g.crumbRects[i], pt)) {
                    self.showLibraryFolder({g.stack.begin(), g.stack.begin() + static_cast<ptrdiff_t>(i)}, false);
                    return 0;
                }
            }
            return 0;
        }
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (hwnd == g.view) {
        switch (msg) {
        case WM_COMMAND: case WM_DRAWITEM: case WM_CTLCOLORSTATIC:
            return self.message(msg, wp, lp);
        case WM_LBUTTONDOWN:
            SetFocus(g.view);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(hwnd, &ps);
            theme::fill(dc, ps.rcPaint, theme::surface);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return TRUE;
        case WM_VSCROLL: {
            SCROLLINFO info{sizeof(info), SIF_PAGE | SIF_TRACKPOS};
            GetScrollInfo(g.view, SB_VERT, &info);
            int64_t scroll = g.scroll;
            switch (LOWORD(wp)) {
            case SB_THUMBTRACK: scroll = info.nTrackPos; break;
            case SB_LINEUP: scroll -= self.libraryPx(34); break;
            case SB_LINEDOWN: scroll += self.libraryPx(34); break;
            case SB_PAGEUP: scroll -= info.nPage; break;
            case SB_PAGEDOWN: scroll += info.nPage; break;
            case SB_TOP: scroll = 0; break;
            case SB_BOTTOM: scroll = g.maxScroll; break;
            default: return 0;
            }
            if (g.scrollTo(scroll)) self.updateGallery();
            return 0;
        }
        case WM_MOUSEWHEEL:
            self.libraryInput(msg, wp);
            return 0;
        }
    } else if (hwnd == g.rename) {
        if (msg == WM_KEYDOWN && wp == VK_RETURN) { self.editClip(ClipEdit::Rename); return 0; }
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { self.endRename(); return 0; }
        if (msg == WM_KILLFOCUS && g.renaming) { self.endRename(); }
    } else {
        const auto slot = static_cast<size_t>(GetWindowLongPtrW(hwnd, GWLP_USERDATA) - 1);
        const bool folder = slot < g.folderTiles.size() && g.folderTiles[slot].hwnd == hwnd;
        const auto index = folder ? (slot < g.folderTiles.size() ? g.folderTiles[slot].index : noClip) :
            (slot < g.tiles.size() ? g.tiles[slot].index : noClip);
        if (msg == WM_MOUSEWHEEL) return SendMessageW(g.view, msg, wp, lp);
        // A folder row only needs its own button click; nothing here applies.
        if (folder) return DefSubclassProc(hwnd, msg, wp, lp);
        if (msg == WM_LBUTTONDOWN && index != noClip) {
            self.selectClip(index);
            g.dragTile = GetCursorPos(&g.dragStart) ? hwnd : nullptr;
        } else if (msg == WM_RBUTTONDOWN && index != noClip) {
            self.selectClip(index);
        } else if (msg == WM_CONTEXTMENU && index < g.clips.size() && !g.editing) {
            self.selectClip(index);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1 && pt.y == -1) {
                RECT tr{};
                GetWindowRect(hwnd, &tr);
                pt = {(tr.left + tr.right) / 2, (tr.top + tr.bottom) / 2};
            }
            if (const HMENU menu = CreatePopupMenu()) {
                constexpr UINT cmdOpen = 1, cmdExplorer = 2, cmdRename = 3, cmdFavorite = 4, cmdDelete = 5;
                AppendMenuW(menu, MF_STRING, cmdOpen, L"Open");
                SetMenuDefaultItem(menu, cmdOpen, FALSE);
                AppendMenuW(menu, MF_STRING, cmdExplorer, L"Show in Explorer");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, cmdRename, L"Rename");
                AppendMenuW(menu, MF_STRING, cmdFavorite,
                            g.clips[index].info.favorite ? L"Unfavorite" : L"Favorite");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, cmdDelete, L"Delete");
                const auto cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                                pt.x, pt.y, 0, self.window, nullptr);
                DestroyMenu(menu);
                if (cmd == cmdOpen) self.openClip();
                else if (cmd == cmdExplorer && index < g.clips.size()) {
                    const auto path = g.clips[index].info.path;
                    PIDLIST_ABSOLUTE pidl = nullptr;
                    bool shown = false;
                    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
                        shown = SUCCEEDED(SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0));
                        CoTaskMemFree(pidl);
                    }
                    if (!shown) {
                        const auto args = L"/select,\"" + path.wstring() + L"\"";
                        if (!shellExecuteSucceeded(ShellExecuteW(self.window, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL)))
                            self.setStatus(L"Could not open folder in Explorer.");
                    }
                } else if (cmd == cmdRename) self.beginRename();
                else if (cmd == cmdFavorite) self.editClip(ClipEdit::Favorite);
                else if (cmd == cmdDelete) self.editClip(ClipEdit::Remove);
            }
            return 0;
        } else if (msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE || msg == WM_NCDESTROY) {
            if (g.dragTile == hwnd) g.dragTile = nullptr;
        } else if (msg == WM_LBUTTONDBLCLK && index != noClip) {
            self.selectClip(index);
            self.openClip();
            return 0;
        } else if (msg == WM_MOUSEMOVE && g.dragTile == hwnd && GetCapture() == hwnd &&
                   !g.dragActive && !g.editing && (wp & MK_LBUTTON) && index < g.clips.size()) {
            POINT point{};
            if (GetCursorPos(&point) &&
                (std::abs(point.x - g.dragStart.x) >= GetSystemMetricsForDpi(SM_CXDRAG, self.dpi) ||
                 std::abs(point.y - g.dragStart.y) >= GetSystemMetricsForDpi(SM_CYDRAG, self.dpi))) {
                const auto path = g.clips[index].info.path;
                g.dragActive = true;
                g.cancelDrag();
                const auto success = shellDrag(hwnd, path);
                if (self.gallery) self.gallery->dragActive = false;
                if (!success && self.window) self.setStatus(L"Could not start file drag.");
                return 0;
            }
        } else if (msg == WM_MOUSEWHEEL) return SendMessageW(g.view, msg, wp, lp);
        else if (msg == WM_SETFOCUS && index != noClip && self.nav.page == Page::Library && !self.building) self.selectClip(index);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}


bool StudioWindow::Impl::drawGallery(const DRAWITEMSTRUCT& item) {
    if (!gallery || !gallery->view || GetParent(item.hwndItem) != gallery->view) return false;
    auto& g = *gallery;
    const auto slot = static_cast<size_t>(GetWindowLongPtrW(item.hwndItem, GWLP_USERDATA) - 1);
    if (slot < g.folderTiles.size() && g.folderTiles[slot].hwnd == item.hwndItem) {
        const auto index = g.folderTiles[slot].index;
        if (index >= g.folders.size()) return true;
        auto r = item.rcItem;
        auto dc = item.hDC;
        const bool down = (item.itemState & ODS_SELECTED) != 0;
        const bool hasFocus = (item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS && !keyboardCuesHidden();
        theme::fill(dc, r, down ? theme::press : hovered == item.hwndItem ? theme::hover : theme::field);
        theme::frame(dc, r, hasFocus ? theme::accent : theme::rule);
        // Folder glyph: a tab over a body, so no image asset is needed.
        const int icon = libraryPx(16), iconLeft = r.left + libraryPx(8), iconTop = (r.top + r.bottom - libraryPx(14)) / 2;
        theme::fill(dc, {iconLeft, iconTop + libraryPx(2), iconLeft + libraryPx(7), iconTop + libraryPx(6)}, theme::muted);
        theme::fill(dc, {iconLeft, iconTop + libraryPx(4), iconLeft + icon, iconTop + libraryPx(14)}, theme::muted);
        const auto clipCount = g.folders[index].count;
        const auto count = std::to_wstring(clipCount) + (clipCount == 1 ? L" clip" : L" clips");
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, pageFont());
        SIZE extent{};
        GetTextExtentPoint32W(dc, count.c_str(), static_cast<int>(count.size()), &extent);
        const int nameLeft = iconLeft + icon + libraryPx(7);
        SetTextColor(dc, theme::text);
        RECT name{nameLeft, r.top, (std::max)(nameLeft, static_cast<int>(r.right) - libraryPx(8) - static_cast<int>(extent.cx) - libraryPx(8)), r.bottom};
        DrawTextW(dc, g.folders[index].name.c_str(), -1, &name, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetTextColor(dc, theme::muted);
        RECT value{r.left, r.top, r.right - libraryPx(8), r.bottom};
        DrawTextW(dc, count.c_str(), -1, &value, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        return true;
    }
    if (slot >= g.tiles.size() || g.tiles[slot].index >= g.clips.size()) return true;
    const auto index = g.tiles[slot].index;
    const auto& clip = g.clips[index];
    auto r = item.rcItem;
    auto dc = item.hDC;
    const bool selected = index == g.selected;
    const bool down = (item.itemState & ODS_SELECTED) != 0;
    const bool hot = hovered == item.hwndItem;
    const bool focusCue = (item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS && !keyboardCuesHidden();
    theme::fill(dc, r, down ? theme::press : selected ? theme::active : hot ? theme::hover : theme::surface);
    const int line = r.bottom - libraryPx(4);
    const int title = line - libraryPx(42);
    const RECT image{r.left, r.top + libraryPx(5), r.right, title - libraryPx(4)};
    theme::fill(dc, image, theme::field);
    bool drewThumbnail = false;
    if (auto it = g.cache.find(clip.info.path.native());
        it != g.cache.end() && it->second.bytes == clip.info.bytes && it->second.modifiedTime == clip.info.modifiedTime) {
        auto& bitmap = it->second.bitmap;
        if (bitmap.source) {
            drawBitmapFitCached(dc, bitmap.source, bitmap.scaled, bitmap.width, bitmap.height, image);
            drewThumbnail = true;
        }
    }
    if (!drewThumbnail && image.right - image.left >= libraryPx(24) && image.bottom - image.top >= libraryPx(24)) {
        const float cx = (image.left + image.right) * 0.5f;
        const float cy = (image.top + image.bottom) * 0.5f;
        const float radius = static_cast<float>(libraryPx(10));
        const Gdiplus::PointF triangle[]{
            {cx - radius * 0.5f, cy - radius * 0.75f},
            {cx + radius * 0.75f, cy},
            {cx - radius * 0.5f, cy + radius * 0.75f},
        };
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(theme::rule), GetGValue(theme::rule), GetBValue(theme::rule)));
        graphics.FillPolygon(&brush, triangle, 3);
    }
    SelectObject(dc, pageFont());
    SetBkMode(dc, TRANSPARENT);
    if (clip.info.duration100ns > 0 && image.right - image.left >= libraryPx(48) && image.bottom - image.top >= libraryPx(26)) {
        RECT measure{};
        DrawTextW(dc, clip.duration.c_str(), -1, &measure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        const int badgeW = measure.right - measure.left + libraryPx(10);
        const int badgeH = measure.bottom - measure.top + libraryPx(4);
        const int margin = libraryPx(5);
        const RECT badge{image.right - margin - badgeW, image.bottom - margin - badgeH, image.right - margin, image.bottom - margin};
        if (badge.left > image.left + margin && badge.top > image.top + margin) {
            theme::fill(dc, badge, theme::surface);
            theme::frame(dc, badge, theme::rule);
            SetTextColor(dc, theme::text);
            RECT badgeText = badge;
            DrawTextW(dc, clip.duration.c_str(), -1, &badgeText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    theme::frame(dc, image, selected ? theme::accent : (down || hot) ? theme::border : theme::rule);
    if (focusCue) theme::frame(dc, r, theme::accent);
    SetTextColor(dc, theme::text);
    const auto displayTitle = clip.info.path.stem().wstring();
    RECT name{r.left + libraryPx(2), title, r.right - (clip.info.favorite ? libraryPx(21) : libraryPx(2)), title + libraryPx(21)};
    DrawTextW(dc, displayTitle.c_str(), -1, &name, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (clip.info.favorite) {
        RECT star{name.right, name.top, r.right, name.bottom};
        SetTextColor(dc, theme::accent);
        DrawTextW(dc, L"\u2605", -1, &star, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    SetTextColor(dc, theme::muted);
    RECT details{name.left, name.bottom, r.right - libraryPx(2), line};
    DrawTextW(dc, clip.details.c_str(), -1, &details, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    return true;
}

} // namespace backtrack
