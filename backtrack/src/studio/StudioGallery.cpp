#include "studio/StudioWindowImpl.h"
#include "studio/GalleryLayout.h"
#include "studio/StudioTheme.h"

#include "core/LogContext.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"
#include "ui_common/WindowTools.h"

#include <CommCtrl.h>
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

struct GalleryClip {
    ClipInfo info;
    std::wstring name;
    std::wstring details;
};

bool sameMedia(const ClipInfo& a, const ClipInfo& b) {
    return a.bytes == b.bytes && a.modifiedTime == b.modifiedTime && a.path == b.path;
}

bool shellDrag(const std::filesystem::path& path) {
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
    auto* source = new ClipFileDropSource;
    DWORD effect = DROPEFFECT_NONE;
    const auto result = DoDragDrop(object.Get(), source, DROPEFFECT_COPY, &effect);
    source->Release();
    return SUCCEEDED(result);
}
} // namespace

struct StudioWindow::Impl::Gallery {
    struct Tile { HWND hwnd; size_t index = noClip; };
    struct Viewport {
        int width, height, scroll;
        unsigned dpi;
        bool operator==(const Viewport&) const = default;
    };
    struct Scan { uint64_t revision; std::filesystem::path directory; };
    struct Snapshot { uint64_t revision; std::vector<GalleryClip> clips; bool ok = false; };
    using PreviewKey = std::pair<uint64_t, size_t>;
    struct Request {
        uint64_t revision;
        size_t index;
        std::filesystem::path path;
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
                        auto listed = manager.listClips(cancelled);
                        if (cancelled()) continue;
                        result->clips.reserve(listed.size());
                        for (auto& info : listed) {
                            if (cancelled()) break;
                            auto name = info.path.filename().wstring();
                            auto details = durationToText(info.duration100ns) + L"  \u00b7  " + bytesToText(info.bytes);
                            if (!info.tags.empty()) details += L"  \u00b7  " + tagsToText(info.tags);
                            result->clips.push_back({std::move(info), std::move(name), std::move(details)});
                        }
                        result->ok = true;
                    } catch (...) { Logger::instance().warning(L"ui", L"Library scan failed unexpectedly"); }
                    std::scoped_lock lock(mutex);
                    if (cancelled()) continue;
                    snapshot.swap(result);
                    postReady();
                } else {
                    HBITMAP source = nullptr;
                    if (!stop.stop_requested()) {
                        try { source = loadShellThumbnail(thumbnail->path, 480, 270); }
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
    HWND rename = nullptr;
    bool renaming = false;
    HWND empty = nullptr;
    std::array<HWND, 5> actions{};
    std::vector<Tile> tiles;
    std::vector<GalleryClip> clips;
    std::unordered_map<size_t, Bitmap> cache;
    std::filesystem::path directory;
    std::filesystem::path selectionPath;
    StudioGalleryLayout grid;
    std::optional<Viewport> viewport;
    size_t selected = noClip;
    int scroll = 0;
    int maxScroll = 0;
    int wheel = 0;
    POINT dragStart{};
    HWND dragTile = nullptr;
    bool scanning = false;
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
    g.actions[3] = button(L"Refresh", [this] { refreshLibrary(); });
    g.actions[4] = button(L"Delete", [this] { editClip(ClipEdit::Remove); });
    g.view = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"Clip gallery",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL | WS_TABSTOP, 0, 0, 0, 0, body, nullptr, nullptr, nullptr);
    if (!g.view) return;
    // The field replaces the selected tile's name line, so it belongs to the scrolling viewport.
    g.rename = make(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, {}, L"Rename the selected clip without changing its extension.", g.view);
    if (g.rename) SetWindowSubclass(g.rename, galleryProc, 2, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(g.view, galleryProc, 2, reinterpret_cast<DWORD_PTR>(this));
    SetWindowTheme(g.view, L"DarkMode_Explorer", nullptr);
    g.empty = make(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX, {}, L"", g.view);
    if (g.rename) SendMessageW(g.rename, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(5), px(5)));
    if (g.directory != saved.clipDirectory) {
        g.directory = saved.clipDirectory;
        g.clips.clear();
        g.selected = noClip;
        g.scroll = 0;
    }
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
    g.tiles.clear();
    g.viewport.reset();
    g.view = g.empty = g.rename = nullptr;
    g.actions.fill(nullptr);
    g.wheel = 0;
    g.scanning = false;
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

void StudioWindow::Impl::refreshLibrary() {
    if (closing || shuttingDown || nav.page != Page::Library || !gallery || !gallery->view) return;
    auto& g = *gallery;
    g.scanning = true;
    {
        std::scoped_lock lock(g.mutex);
        g.scan = Gallery::Scan{++g.revision, g.directory};
    }
    g.cv.notify_one();
    if (g.clips.empty()) setText(g.empty, L"Loading clips\u2026");
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
                    if (index == g.selected) {
                        g.selected = noClip;
                        g.selectionPath.clear();
                    } else if (g.selected != noClip && index < g.selected) --g.selected;
                    std::erase_if(g.cache, [=](const auto& entry) { return entry.first >= index; });
                    layoutChanged = true;
                }
                break;
            case ClipEdit::Rename:
                if (it != g.clips.end()) {
                    it->info.path.replace_filename(change.name + change.path.extension().wstring());
                    it->name = it->info.path.filename().wstring();
                    if (const auto tile = g.tileFor(index)) {
                        if (g.dragTile == tile) g.cancelDrag();
                        setText(tile, it->name);
                    }
                    if (g.selectionPath == change.path) g.selectionPath = it->info.path;
                    g.cache.erase(index);
                    layoutChanged = true;
                }
                if (!g.scanning) refreshLibrary();
                break;
            }
            setStatus(change.kind == ClipEdit::Remove ? L"Clip deleted" : change.kind == ClipEdit::Rename ? L"Clip renamed" :
                      change.favorite ? L"Clip favorited" : L"Favorite removed");
        }
    }
    if (!g.view) return;
    const bool scanFinished = snapshot && snapshot->revision == g.revision.load();
    if (scanFinished) {
        g.scanning = false;
        if (snapshot->ok) {
            for (auto& tile : g.tiles) {
                const auto i = tile.index;
                if (i == noClip) continue;
                if (i >= snapshot->clips.size() || g.clips[i].info.path != snapshot->clips[i].info.path) invalidateTile(tile);
                else if (g.dragTile == tile.hwnd && !sameMedia(g.clips[i].info, snapshot->clips[i].info)) g.cancelDrag();
            }
            std::erase_if(g.cache, [&](const auto& entry) {
                const auto i = entry.first;
                return !entry.second.source || i >= snapshot->clips.size() || i >= g.clips.size() ||
                    !sameMedia(g.clips[i].info, snapshot->clips[i].info);
            });
            g.clips = std::move(snapshot->clips);
            if (g.selected >= g.clips.size() || g.clips[g.selected].info.path != g.selectionPath) {
                g.selected = noClip;
                if (!g.selectionPath.empty()) {
                    const auto it = std::ranges::find(g.clips, g.selectionPath, [](const auto& clip) -> const auto& { return clip.info.path; });
                    if (it != g.clips.end()) g.selected = static_cast<size_t>(it - g.clips.begin());
                }
            }
            layoutChanged = true;
        } else {
            if (g.clips.empty()) setText(g.empty, L"No clips");
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
        if (index >= g.clips.size() || g.clips[index].info.path != preview.request.path ||
            preview.request.revision != g.previewRevision || index < g.first || index >= g.last) continue;
        const auto [entry, inserted] = g.cache.try_emplace(index, std::move(preview.bitmap));
        if (inserted && entry->second.source) {
            if (const auto tile = g.tileFor(index)) InvalidateRect(tile, nullptr, FALSE);
        }
    }
    if (edit || scanFinished) selectClip(g.selected);
    if (layoutChanged) updateGallery();
    if (edit || snapshot) g.cv.notify_one();
}

void StudioWindow::Impl::layoutGallery() {
    if (!gallery || !gallery->view) return;
    auto& g = *gallery;
    RECT r{};
    GetClientRect(body, &r);
    const int pad = px(theme::inset), gap = px(theme::gap);
    const int width = (std::max)(0L, r.right - pad * 2);
    const int row = px(30), y = px(9), buttonWidth = px(92);
    std::vector<WindowMove> moves;
    for (int i = 0; i < 3; ++i) moves.push_back({g.actions[i], pad + i * (buttonWidth + gap), y, buttonWidth, row});
    moves.push_back({g.actions[3], pad + width - 2 * buttonWidth - gap, y, buttonWidth, row});
    moves.push_back({g.actions[4], pad + width - buttonWidth, y, buttonWidth, row});
    const int top = y + row + gap;
    moves.push_back({g.view, 0, top, r.right, (std::max)(0L, r.bottom - top)});
    applyWindowMoves(moves);
    updateGallery();
}

void StudioWindow::Impl::updateGallery() {
    if (!gallery || !gallery->view || shuttingDown) return;
    auto& g = *gallery;
    RECT r{};
    GetClientRect(g.view, &r);
    const int width = r.right, height = r.bottom;
    const int pad = px(theme::inset), gap = px(theme::gap);
    const int columns = std::clamp((width + GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) - pad * 2 + gap) / px(210), 1, 5);
    const int tileWidth = (std::max)(1, (width - pad * 2 - (columns - 1) * gap) / columns);
    const int rowHeight = (std::min)(px(250), tileWidth * 9 / 16 + px(59));
    int64_t position = g.scroll;
    if (g.grid.columns != columns || g.grid.rowHeight != rowHeight)
        position = position / g.grid.rowHeight * g.grid.columns / columns * rowHeight;
    g.grid = {columns, rowHeight};
    const auto rowCount = g.grid.rowCount(g.clips.size());
    const int bottom = static_cast<int>((std::min)(rowCount, static_cast<size_t>(INT_MAX / rowHeight))) * rowHeight;
    g.maxScroll = (std::max)(0, bottom - height);
    g.scrollTo(position);
    const bool placeRename = g.renaming && g.rename && g.selected < g.clips.size();
    if (placeRename) {
        const int nameTop = rowHeight - px(4) - px(42);
        SetWindowPos(g.rename, HWND_TOP,
            pad + static_cast<int>(g.selected % g.grid.columns) * (tileWidth + gap) + px(2),
            static_cast<int>(g.selected / g.grid.columns) * rowHeight - g.scroll + nameTop,
            (std::max)(px(30), tileWidth - px(4)), px(21), SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
    }
    const Gallery::Viewport viewport{width, height, g.scroll, dpi};
    if (g.viewport == viewport) {
        if (placeRename) redrawWindowAndChildren(g.view);
        return;
    }
    g.viewport = viewport;
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL, 0,
                    (std::max)(0, bottom - 1), static_cast<UINT>(r.bottom), g.scroll};
    SetScrollInfo(g.view, SB_VERT, &info, FALSE);
    if (g.clips.empty()) {
        setText(g.empty, g.scanning ? L"Loading clips\u2026" : L"No clips");
        moveWindowIfChanged(g.empty, 0, 0, r.right, px(52));
        ShowWindow(g.empty, SW_SHOWNA);
    } else ShowWindow(g.empty, SW_HIDE);
    const auto [first, drawnEnd] = g.grid.visible(g.clips.size(), g.scroll, height);
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
            static_cast<int>(index / columns) * rowHeight - g.scroll, tileWidth, rowHeight});
    }
    applyWindowMoves(moves);
    for (const auto& tile : g.tiles) if (tile.index != noClip && !(GetWindowLongPtrW(tile.hwnd, GWL_STYLE) & WS_VISIBLE))
        ShowWindow(tile.hwnd, SW_SHOWNA);
    if (GetFocus() == g.view) {
        if (const auto tile = g.tileFor(focusClip)) SetFocus(tile);
    }
    redrawWindowAndChildren(g.view);
    if (closing) return;
    const auto last = (std::min)(g.grid.visible(g.clips.size(), g.scroll, height, 1).second, first + thumbnailBudget);
    if (first == g.first && last == g.last) return;
    std::erase_if(g.cache, [&](const auto& item) { return item.first < first || item.first >= last; });
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
        for (auto i = first; i < last; ++i) if (!pending[i - first] && !g.cache.contains(i)) {
            g.requests.push_back({g.previewRevision, i, g.clips[i].info.path});
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
    for (const auto i : {0, 1, 2, 3, 4}) EnableWindow(g.actions[i], selected && !g.editing);
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
        const auto y = static_cast<int64_t>(index / g.grid.columns) * g.grid.rowHeight;
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

void StudioWindow::Impl::beginRename() {
    if (closing || shuttingDown || !gallery || !gallery->view || !gallery->rename || gallery->editing ||
        gallery->selected >= gallery->clips.size()) return;
    auto& g = *gallery;
    g.renaming = true;
    setText(g.rename, g.clips[g.selected].info.path.stem().wstring());
    ShowWindow(g.rename, SW_SHOWNA);
    updateGallery();
    SetFocus(g.rename);
    SendMessageW(g.rename, EM_SETSEL, 0, -1);
}

void StudioWindow::Impl::endRename() {
    if (!gallery || !gallery->view || !gallery->rename) return;
    auto& g = *gallery;
    g.renaming = false;
    ShowWindow(g.rename, SW_HIDE);
    if (GetFocus() == g.rename) SetFocus(g.view);
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
        change.name = trimText(readText(g.rename));
        if (change.name.empty()) { setStatus(L"Enter a clip name."); return; }
        endRename();
    } else if (kind == ClipEdit::Remove) {
        const auto view = g.view;
        const auto target = info;
        if (MessageBoxW(window, (L"Delete " + target.path.filename().wstring() + L"?").c_str(), L"Delete clip",
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
        if (g.scanning && !g.scan) g.scan = Gallery::Scan{++g.revision, g.directory};
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
    if (msg.wParam == VK_F5 || (isControlDown() && msg.wParam == L'R')) { refreshLibrary(); return true; }
    if ((isControlDown() && msg.wParam == L'O') || (inGallery && msg.wParam == VK_RETURN)) { openClip(); return true; }
    if (msg.wParam == VK_DELETE) { editClip(ClipEdit::Remove); return true; }
    if (msg.wParam == VK_F2 && g.selected < g.clips.size()) { beginRename(); return true; }
    if (!inGallery || g.clips.empty()) return false;
    const auto current = static_cast<ptrdiff_t>(g.selected == noClip ? 0 : g.selected);
    const auto last = static_cast<ptrdiff_t>(g.clips.size() - 1);
    ptrdiff_t next = current;
    switch (msg.wParam) {
    case VK_LEFT: --next; break;
    case VK_RIGHT: ++next; break;
    case VK_UP: next -= g.grid.columns; break;
    case VK_DOWN: next += g.grid.columns; break;
    case VK_HOME: next = 0; break;
    case VK_END: next = last; break;
    case VK_PRIOR: case VK_NEXT: {
        RECT r{};
        GetClientRect(g.view, &r);
        next += (msg.wParam == VK_PRIOR ? -1 : 1) * g.grid.columns * (std::max)(1L, r.bottom / g.grid.rowHeight - 1);
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
    if (hwnd == g.view) {
        switch (msg) {
        case WM_COMMAND: case WM_DRAWITEM: case WM_CTLCOLORSTATIC:
            return self.message(msg, wp, lp);
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
            case SB_LINEUP: scroll -= self.px(34); break;
            case SB_LINEDOWN: scroll += self.px(34); break;
            case SB_PAGEUP: scroll -= info.nPage; break;
            case SB_PAGEDOWN: scroll += info.nPage; break;
            case SB_TOP: scroll = 0; break;
            case SB_BOTTOM: scroll = g.maxScroll; break;
            default: return 0;
            }
            if (g.scrollTo(scroll)) self.updateGallery();
            return 0;
        }
        case WM_MOUSEWHEEL: {
            g.wheel += GET_WHEEL_DELTA_WPARAM(wp);
            const auto steps = g.wheel / WHEEL_DELTA;
            g.wheel %= WHEEL_DELTA;
            if (!steps) return 0;
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            const int64_t distance = lines == WHEEL_PAGESCROLL ? (g.viewport ? g.viewport->height : 0) :
                self.px(25) * static_cast<int>((std::min)(lines, 100U));
            if (g.scrollTo(g.scroll - steps * distance)) self.updateGallery();
            return 0;
        }
        }
    } else if (hwnd == g.rename) {
        if (msg == WM_KEYDOWN && wp == VK_RETURN) { self.editClip(ClipEdit::Rename); return 0; }
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { self.endRename(); return 0; }
    } else {
        const auto slot = static_cast<size_t>(GetWindowLongPtrW(hwnd, GWLP_USERDATA) - 1);
        const auto index = slot < g.tiles.size() ? g.tiles[slot].index : noClip;
        if (msg == WM_LBUTTONDOWN && index != noClip) {
            self.selectClip(index);
            g.dragTile = GetCursorPos(&g.dragStart) ? hwnd : nullptr;
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
                const auto success = shellDrag(path);
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
    if (slot >= g.tiles.size() || g.tiles[slot].index >= g.clips.size()) return true;
    const auto index = g.tiles[slot].index;
    const auto& clip = g.clips[index];
    auto r = item.rcItem;
    auto dc = item.hDC;
    theme::fill(dc, r, theme::surface);
    const int line = r.bottom - px(4);
    const int title = line - px(42);
    const RECT image{r.left, r.top + px(5), r.right, title - px(4)};
    theme::fill(dc, image, theme::field);
    if (auto it = g.cache.find(index); it != g.cache.end()) {
        auto& bitmap = it->second;
        if (bitmap.source) drawBitmapFitCached(dc, bitmap.source, bitmap.scaled, bitmap.width, bitmap.height, image);
    }
    const auto selected = index == g.selected;
    const bool hasFocus = (item.itemState & ODS_FOCUS) != 0;
    if (selected || hasFocus || hovered == item.hwndItem) theme::frame(dc, image, selected || hasFocus ? theme::accent : theme::rule);
    theme::fill(dc, {r.left, line, r.right, line + 1}, theme::muted);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme::text);
    SelectObject(dc, font);
    RECT name{r.left + px(2), title, r.right - (clip.info.favorite ? px(21) : px(2)), title + px(21)};
    DrawTextW(dc, clip.name.c_str(), -1, &name, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (clip.info.favorite) {
        RECT star{name.right, name.top, r.right, name.bottom};
        SetTextColor(dc, theme::accent);
        DrawTextW(dc, L"\u2605", -1, &star, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    SetTextColor(dc, theme::muted);
    RECT details{name.left, name.bottom, r.right - px(2), line};
    DrawTextW(dc, clip.details.c_str(), -1, &details, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    return true;
}

} // namespace backtrack
