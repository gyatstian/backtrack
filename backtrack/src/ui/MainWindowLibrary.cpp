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

namespace {

int galleryTileWidth(const RECT& row) {
    const int rowWidth = std::max(1, static_cast<int>(row.right - row.left));
    return std::max(1, (rowWidth - kGalleryTileGap * (kGalleryClipColumns + 1)) / kGalleryClipColumns);
}

} // namespace

RECT MainWindow::galleryTileRect(const RECT& row, int column) {
    const int tileWidth = galleryTileWidth(row);
    const int left = static_cast<int>(row.left) + kGalleryTileGap + column * (tileWidth + kGalleryTileGap);
    return RECT{
        left,
        static_cast<int>(row.top) + kGalleryTileGap,
        left + tileWidth,
        static_cast<int>(row.bottom) - kGalleryTileGap};
}

int MainWindow::galleryTileColumnAt(const RECT& row, int x) {
    const int offset = x - static_cast<int>(row.left) - kGalleryTileGap;
    // A gap belongs to the tile on its left, so every point in the row resolves
    // to the tile the painter drew there instead of the next column.
    const int column = offset < 0 ? 0 : offset / (galleryTileWidth(row) + kGalleryTileGap);
    return std::clamp(column, 0, kGalleryClipColumns - 1);
}

void MainWindow::setLibraryViewMode(LibraryViewMode mode) {
    if (libraryViewMode_ == mode && clipList_) {
        return;
    }
    libraryViewMode_ = mode;
    selectedClipIndex_ = static_cast<size_t>(-1);
    clipListWheelRemainder_ = 0;
    // Persist the view flag against the in-memory settings, never against
    // controller_.settings(): during a pending ApplySettings the controller
    // still holds pre-save values, and saving them here used to revert the
    // settings file the user had just written.
    settings_.libraryGalleryView = mode == LibraryViewMode::Gallery;
    settingsStore_.save(settings_);
    savedSettings_ = settings_;
    Logger::instance().info(L"ui", std::wstring(L"Library view changed to ") + (mode == LibraryViewMode::Gallery ? L"gallery" : L"list"));
    if (clipList_) {
        setListItemHeightIfChanged(clipList_, clipListItemHeight_, mode == LibraryViewMode::Gallery ? kGalleryClipMinimumItemHeight : kListClipItemHeight);
        refreshClips();
        layoutCurrentPage();
        InvalidateRect(clipList_, nullptr, FALSE);
    }
}

void MainWindow::clearClipThumbnails() {
    thumbnailGeneration_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::scoped_lock lock(thumbnailQueueMutex_);
        thumbnailQueue_.clear();
    }
    clipThumbnails_.clear();
}

void MainWindow::releaseClipThumbnailCache() {
    clearClipThumbnails();
    for (auto& [key, entry] : thumbnailCache_) {
        if (entry.bitmap) {
            DeleteObject(entry.bitmap);
        }
        if (entry.tileBitmap) {
            DeleteObject(entry.tileBitmap);
        }
    }
    thumbnailCache_.clear();
}

void MainWindow::pruneClipThumbnailCache() {
    std::unordered_set<std::wstring> liveKeys;
    liveKeys.reserve(clips_.size());
    for (const auto& clip : clips_) {
        liveKeys.insert(thumbnailCacheKey(clip.path));
    }

    for (auto it = thumbnailCache_.begin(); it != thumbnailCache_.end();) {
        if (liveKeys.contains(it->first)) {
            ++it;
            continue;
        }
        if (it->second.bitmap) {
            DeleteObject(it->second.bitmap);
        }
        if (it->second.tileBitmap) {
            DeleteObject(it->second.tileBitmap);
        }
        it = thumbnailCache_.erase(it);
    }
}

void MainWindow::loadClipThumbnails() {
    clearClipThumbnails();
    // Prune before the Gallery check so deleted/renamed clips release their
    // cached bitmaps regardless of the active library view mode.
    pruneClipThumbnailCache();
    if (libraryViewMode_ != LibraryViewMode::Gallery) {
        return;
    }
    clipThumbnails_.resize(clips_.size());

    std::deque<ThumbnailRequest> requests;
    const uint64_t generation = thumbnailGeneration_.load(std::memory_order_acquire);
    for (size_t index = 0; index < clips_.size(); ++index) {
        const auto& clip = clips_[index];
        const std::wstring key = thumbnailCacheKey(clip.path);
        auto cache = thumbnailCache_.find(key);
        if (cache != thumbnailCache_.end()) {
            auto& entry = cache->second;
            if (entry.bytes == clip.bytes && entry.modifiedTime == clip.modifiedTime) {
                clipThumbnails_[index].bitmap = entry.bitmap;
                clipThumbnails_[index].tileBitmap = entry.tileBitmap;
                clipThumbnails_[index].tileWidth = entry.tileWidth;
                clipThumbnails_[index].tileHeight = entry.tileHeight;
                clipThumbnails_[index].unavailable = entry.unavailable;
                continue;
            }
            if (entry.bitmap) {
                DeleteObject(entry.bitmap);
            }
            if (entry.tileBitmap) {
                DeleteObject(entry.tileBitmap);
            }
            thumbnailCache_.erase(cache);
        }

        clipThumbnails_[index].loading = true;
        ThumbnailRequest request;
        request.generation = generation;
        request.clipIndex = index;
        request.path = clip.path;
        request.modifiedTime = clip.modifiedTime;
        request.bytes = clip.bytes;
        requests.push_back(std::move(request));
    }

    if (requests.empty()) {
        return;
    }

    // Decode what is on screen first: with a large uncached library the
    // visible tiles would otherwise wait behind every off-screen entry.
    if (clipList_) {
        const LRESULT top = SendMessageW(clipList_, LB_GETTOPINDEX, 0, 0);
        RECT client{};
        GetClientRect(clipList_, &client);
        const int itemHeight = std::max(1, clipListItemHeight_ > 0 ? clipListItemHeight_ : kGalleryClipMinimumItemHeight);
        const int visibleRows = std::max(1, static_cast<int>(client.bottom - client.top) / itemHeight);
        if (top != LB_ERR && top >= 0) {
            const size_t firstVisible = static_cast<size_t>(top) * kGalleryClipColumns;
            const size_t lastVisible = firstVisible + static_cast<size_t>(visibleRows) * kGalleryClipColumns;
            std::stable_partition(requests.begin(), requests.end(), [&](const ThumbnailRequest& request) {
                return request.clipIndex >= firstVisible && request.clipIndex < lastVisible;
            });
        }
    }

    startThumbnailWorker();
    {
        std::scoped_lock lock(thumbnailQueueMutex_);
        if (thumbnailWorkerStopping_) {
            return;
        }
        for (auto& request : requests) {
            thumbnailQueue_.push_back(std::move(request));
        }
    }
    thumbnailQueueCv_.notify_one();
}

void MainWindow::refreshThumbnailsAfterLibraryEdit() {
    // Same per-index validation as handleClipThumbnailReady already drops
    // stale results, so reusing the current generation is safe: duplicates
    // self-discard instead of starving the worker like a bump would.
    pruneClipThumbnailCache();
    if (libraryViewMode_ != LibraryViewMode::Gallery || clips_.empty()) {
        clipThumbnails_.clear();
        return;
    }
    clipThumbnails_.resize(clips_.size());
    const uint64_t generation = thumbnailGeneration_.load(std::memory_order_acquire);
    std::deque<ThumbnailRequest> requests;
    for (size_t index = 0; index < clips_.size(); ++index) {
        const auto& clip = clips_[index];
        const std::wstring key = thumbnailCacheKey(clip.path);
        const auto cache = thumbnailCache_.find(key);
        if (cache != thumbnailCache_.end() &&
            cache->second.bytes == clip.bytes &&
            cache->second.modifiedTime == clip.modifiedTime) {
            auto& slot = clipThumbnails_[index];
            slot.bitmap = cache->second.bitmap;
            slot.tileBitmap = cache->second.tileBitmap;
            slot.tileWidth = cache->second.tileWidth;
            slot.tileHeight = cache->second.tileHeight;
            slot.loading = false;
            slot.unavailable = cache->second.unavailable;
            continue;
        }
        clipThumbnails_[index] = ClipThumbnail{};
        clipThumbnails_[index].loading = true;
        ThumbnailRequest request;
        request.generation = generation;
        request.clipIndex = index;
        request.path = clip.path;
        request.modifiedTime = clip.modifiedTime;
        request.bytes = clip.bytes;
        requests.push_back(std::move(request));
    }
    if (requests.empty()) {
        return;
    }
    startThumbnailWorker();
    {
        std::scoped_lock lock(thumbnailQueueMutex_);
        if (thumbnailWorkerStopping_) {
            return;
        }
        for (auto& request : requests) {
            thumbnailQueue_.push_back(std::move(request));
        }
    }
    thumbnailQueueCv_.notify_one();
}

void MainWindow::startThumbnailWorker() {
    if (thumbnailWorker_.joinable()) {
        return;
    }

    {
        std::scoped_lock lock(thumbnailQueueMutex_);
        thumbnailWorkerStopping_ = false;
        thumbnailQueue_.clear();
    }
    thumbnailWorker_ = std::thread(&MainWindow::thumbnailWorkerLoop, this);
}

void MainWindow::stopThumbnailWorker() {
    thumbnailGeneration_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::scoped_lock lock(thumbnailQueueMutex_);
        thumbnailWorkerStopping_ = true;
        thumbnailQueue_.clear();
    }
    thumbnailQueueCv_.notify_all();
    if (thumbnailWorker_.joinable()) {
        thumbnailWorker_.join();
    }

    MSG message{};
    while (window_ && PeekMessageW(&message, window_, kClipThumbnailReadyMessage, kClipThumbnailReadyMessage, PM_REMOVE)) {
        auto* result = reinterpret_cast<ThumbnailResult*>(message.lParam);
        if (result) {
            if (result->bitmap) {
                DeleteObject(result->bitmap);
            }
            delete result;
        }
    }
}

void MainWindow::thumbnailWorkerLoop() {
    setThreadDescriptionSafe(L"Backtrack thumbnails");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    for (;;) {
        ThumbnailRequest request;
        {
            std::unique_lock lock(thumbnailQueueMutex_);
            thumbnailQueueCv_.wait(lock, [&] {
                return thumbnailWorkerStopping_ || !thumbnailQueue_.empty();
            });
            if (thumbnailWorkerStopping_ && thumbnailQueue_.empty()) {
                break;
            }
            request = std::move(thumbnailQueue_.front());
            thumbnailQueue_.pop_front();
        }

        if (request.generation != thumbnailGeneration_.load(std::memory_order_acquire)) {
            continue;
        }

        HBITMAP bitmap = loadShellThumbnail(request.path, kGalleryThumbnailWidth, kGalleryThumbnailHeight);
        bool stopping = false;
        {
            std::scoped_lock lock(thumbnailQueueMutex_);
            stopping = thumbnailWorkerStopping_;
        }
        if (stopping || request.generation != thumbnailGeneration_.load(std::memory_order_acquire)) {
            if (bitmap) {
                DeleteObject(bitmap);
            }
            continue;
        }

        auto result = std::make_unique<ThumbnailResult>();
        result->generation = request.generation;
        result->clipIndex = request.clipIndex;
        result->path = std::move(request.path);
        result->modifiedTime = request.modifiedTime;
        result->bytes = request.bytes;
        result->bitmap = bitmap;

        auto* rawResult = result.release();
        if (!window_ ||
            !IsWindow(window_) ||
            !PostMessageW(window_, kClipThumbnailReadyMessage, 0, reinterpret_cast<LPARAM>(rawResult))) {
            if (rawResult->bitmap) {
                DeleteObject(rawResult->bitmap);
            }
            delete rawResult;
        }
    }

    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
}

void MainWindow::handleClipThumbnailReady(ThumbnailResult& result) {
    auto deleteResultBitmap = [&] {
        if (result.bitmap) {
            DeleteObject(result.bitmap);
            result.bitmap = nullptr;
        }
    };

    if (result.generation != thumbnailGeneration_.load(std::memory_order_acquire) ||
        libraryViewMode_ != LibraryViewMode::Gallery ||
        result.clipIndex >= clips_.size() ||
        result.clipIndex >= clipThumbnails_.size()) {
        deleteResultBitmap();
        return;
    }

    const auto& clip = clips_[result.clipIndex];
    const std::wstring key = thumbnailCacheKey(result.path);
    if (thumbnailCacheKey(clip.path) != key ||
        clip.bytes != result.bytes ||
        clip.modifiedTime != result.modifiedTime) {
        deleteResultBitmap();
        return;
    }

    auto& entry = thumbnailCache_[key];
    if (entry.bitmap && entry.bitmap != result.bitmap) {
        DeleteObject(entry.bitmap);
    }
    if (entry.tileBitmap) {
        // Tile copies are derived from the base bitmap; the new bitmap needs a
        // fresh one at whatever size is drawn next.
        DeleteObject(entry.tileBitmap);
        entry.tileBitmap = nullptr;
        entry.tileWidth = 0;
        entry.tileHeight = 0;
    }
    entry.modifiedTime = result.modifiedTime;
    entry.bytes = result.bytes;
    entry.bitmap = result.bitmap;
    entry.unavailable = result.bitmap == nullptr;
    result.bitmap = nullptr;

    auto& slot = clipThumbnails_[result.clipIndex];
    slot.bitmap = entry.bitmap;
    slot.tileBitmap = entry.tileBitmap;
    slot.tileWidth = entry.tileWidth;
    slot.tileHeight = entry.tileHeight;
    slot.loading = false;
    slot.unavailable = entry.unavailable;

    invalidateClipRow(result.clipIndex);
}

size_t MainWindow::clipIndexFromListItem(UINT itemId) const {
    if (itemId == static_cast<UINT>(-1)) {
        return static_cast<size_t>(-1);
    }
    const size_t row = static_cast<size_t>(itemId);
    if (libraryViewMode_ == LibraryViewMode::Gallery) {
        const size_t index = row * kGalleryClipColumns;
        return index < clips_.size() ? index : static_cast<size_t>(-1);
    }
    return row < clips_.size() ? row : static_cast<size_t>(-1);
}

size_t MainWindow::clipIndexAtPoint(POINT point) const {
    if (!clipList_) {
        return static_cast<size_t>(-1);
    }

    const DWORD hit = static_cast<DWORD>(SendMessageW(
        clipList_, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y)));
    if (HIWORD(hit) != 0) {
        return static_cast<size_t>(-1);
    }

    const int row = LOWORD(hit);
    if (row < 0) {
        return static_cast<size_t>(-1);
    }
    if (libraryViewMode_ != LibraryViewMode::Gallery) {
        return clipIndexFromListItem(static_cast<UINT>(row));
    }

    // Same rect the painter used for this row, not the client rect: the tile
    // geometry below is only correct for the row it was drawn into.
    RECT rowRect{};
    if (SendMessageW(clipList_, LB_GETITEMRECT, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&rowRect)) == LB_ERR) {
        GetClientRect(clipList_, &rowRect);
    }
    const int column = galleryTileColumnAt(rowRect, static_cast<int>(point.x));
    // Gaps belong to no tile: a click between tiles or on the empty part of a
    // partial last row must not resolve to a neighboring clip.
    const RECT tile = galleryTileRect(rowRect, column);
    if (!PtInRect(&tile, point)) {
        return static_cast<size_t>(-1);
    }
    const size_t index = static_cast<size_t>(row) * kGalleryClipColumns + static_cast<size_t>(column);
    return index < clips_.size() ? index : static_cast<size_t>(-1);
}

void MainWindow::armClipDrag(POINT point) {
    clipDragArmed_ = false;
    clipDragPath_.clear();
    suppressGalleryFollow_ = false;
    const size_t index = clipIndexAtPoint(point);
    if (index >= clips_.size()) {
        // Gap or empty area in Gallery: the listbox still moves its own row
        // cursor after this returns, which would make LBN_SELCHANGE follow to
        // the row's first tile. Record the pre-click cursor so the follow can
        // restore it instead - but only when the click actually moves the
        // cursor, or a stale flag would eat the next keyboard selection.
        if (libraryViewMode_ == LibraryViewMode::Gallery && clipList_) {
            const DWORD hit = static_cast<DWORD>(SendMessageW(
                clipList_, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y)));
            const LRESULT current = SendMessageW(clipList_, LB_GETCURSEL, 0, 0);
            if (HIWORD(hit) == 0 && static_cast<int>(LOWORD(hit)) != static_cast<int>(current)) {
                gapPrevRow_ = static_cast<int>(current);
                suppressGalleryFollow_ = true;
            }
        }
        return;
    }

    if (selectedClipIndex_ != index) {
        const size_t previousIndex = selectedClipIndex_;
        selectedClipIndex_ = index;
        invalidateClipRow(previousIndex);
        invalidateClipRow(selectedClipIndex_);
    }
    SendMessageW(clipList_, LB_SETCURSEL,
        static_cast<WPARAM>(libraryViewMode_ == LibraryViewMode::Gallery ? index / kGalleryClipColumns : index), 0);
    // LB_SETCURSEL sends no notification, so sync dependents here instead of
    // waiting for a selection message that never comes.
    onClipSelectionChanged();
    clipDragStartPoint_ = point;
    clipDragPath_ = clips_[index].path;
    clipDragArmed_ = true;
}

void MainWindow::beginClipDrag() {
    clipDragArmed_ = false;
    const std::filesystem::path path = std::move(clipDragPath_);
    clipDragPath_.clear();
    if (path.empty() || !std::filesystem::is_regular_file(path)) {
        setStatus(L"Clip is no longer available to drag");
        return;
    }
    Logger::instance().info(L"ui", L"Starting Shell drag for clip: " + path.wstring());

    PIDLIST_ABSOLUTE absolutePidl = nullptr;
    HRESULT result = SHParseDisplayName(path.c_str(), nullptr, &absolutePidl, 0, nullptr);
    PIDLIST_ABSOLUTE parentPidl = nullptr;
    Microsoft::WRL::ComPtr<IDataObject> dataObject;
    if (SUCCEEDED(result) && absolutePidl) {
        parentPidl = ILCloneFull(absolutePidl);
        const PCUITEMID_CHILD childPidl = ILFindLastID(absolutePidl);
        if (parentPidl && childPidl && ILRemoveLastID(parentPidl)) {
            PCUITEMID_CHILD children[] = {childPidl};
            result = SHCreateDataObject(parentPidl, 1, children, nullptr, IID_IDataObject,
                reinterpret_cast<void**>(dataObject.GetAddressOf()));
        } else {
            result = E_FAIL;
        }
    }
    if (parentPidl) {
        CoTaskMemFree(parentPidl);
    }
    if (absolutePidl) {
        CoTaskMemFree(absolutePidl);
    }
    if (FAILED(result) || !dataObject) {
        Logger::instance().warning(L"ui", L"Could not create Shell drag data for clip: " + path.wstring());
        setStatus(L"Could not start file drag");
        return;
    }

    auto* dropSource = new ClipFileDropSource();
    DWORD effect = DROPEFFECT_NONE;
    result = DoDragDrop(dataObject.Get(), dropSource, DROPEFFECT_COPY, &effect);
    dropSource->Release();
    if (result == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE) {
        setStatus(L"Dropped " + path.filename().wstring());
    } else if (FAILED(result)) {
        Logger::instance().warning(L"ui", L"Shell file drag failed for " + path.wstring() + L"; HRESULT=" + std::to_wstring(result));
        setStatus(L"Could not start file drag");
    }
}

void MainWindow::invalidateClipRow(size_t clipIndex) {
    if (!clipList_ || clipIndex >= clips_.size()) {
        return;
    }

    const WPARAM row = libraryViewMode_ == LibraryViewMode::Gallery
        ? static_cast<WPARAM>(clipIndex / kGalleryClipColumns)
        : static_cast<WPARAM>(clipIndex);
    RECT rowRect{};
    if (SendMessageW(clipList_, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&rowRect)) != LB_ERR) {
        InvalidateRect(clipList_, &rowRect, FALSE);
    } else {
        InvalidateRect(clipList_, nullptr, FALSE);
    }
}

void MainWindow::refreshClips() {
    if (!clipList_) {
        return;
    }

    clipManager_.setDirectory(controller_.settings().clipDirectory);
    clips_ = clipManager_.listClips();
    selectedClipIndex_ = static_cast<size_t>(-1);
    loadClipThumbnails();
    populateClipListRows();
    if (clips_.empty()) {
        setText(renameEdit_, L"");
        setStatus(L"No clips found");
        return;
    }

    Logger::instance().info(L"ui", L"Library refreshed: " + std::to_wstring(clips_.size()) + L" clips in " + controller_.settings().clipDirectory.wstring());
    std::wstring status = std::to_wstring(clips_.size()) + (clips_.size() == 1 ? L" clip found" : L" clips found");
    const bool loadingPreviews = std::any_of(clipThumbnails_.begin(), clipThumbnails_.end(), [](const ClipThumbnail& thumbnail) {
        return thumbnail.loading;
    });
    if (loadingPreviews) {
        status += L"; loading previews";
    }
    setStatus(status);
}

void MainWindow::populateClipListRows() {
    if (!clipList_) {
        return;
    }

    ScopedRedrawLock redraw(clipList_);
    SendMessageW(clipList_, LB_RESETCONTENT, 0, 0);
    if (clips_.empty()) {
        SendMessageW(clipList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No clips yet"));
        updateLibraryActionButtons();
        return;
    }

    if (libraryViewMode_ == LibraryViewMode::Gallery) {
        const size_t rows = (clips_.size() + kGalleryClipColumns - 1) / kGalleryClipColumns;
        for (size_t row = 0; row < rows; ++row) {
            const auto index = SendMessageW(clipList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L""));
            if (index != LB_ERR) {
                SendMessageW(clipList_, LB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(row));
            }
        }
    } else {
        for (const auto& clip : clips_) {
            std::wstring row = clip.favorite ? L"* " : L"  ";
            row += clip.path.filename().wstring();
            const std::wstring tagText = tagsToText(clip.tags);
            if (!tagText.empty()) {
                row += L" [" + tagText + L"]";
            }
            const auto index = SendMessageW(clipList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
            if (index != LB_ERR) {
                SendMessageW(clipList_, LB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(index));
            }
        }
    }

    if (selectedClipIndex_ < clips_.size()) {
        SendMessageW(clipList_, LB_SETCURSEL,
            static_cast<WPARAM>(libraryViewMode_ == LibraryViewMode::Gallery ? selectedClipIndex_ / kGalleryClipColumns : selectedClipIndex_), 0);
    }
    InvalidateRect(clipList_, nullptr, FALSE);
    updateLibraryActionButtons();
}

void MainWindow::updateLibraryActionButtons() {
    // The painter already renders a disabled look; use it instead of leaving
    // dead-end buttons enabled on an empty or unselected library.
    const BOOL enabled = (selectedClipIndex_ < clips_.size() && !clips_.empty()) ? TRUE : FALSE;
    if (pageHost_) {
        EnableWindow(GetDlgItem(pageHost_, kDeleteClipButtonId), enabled);
        EnableWindow(GetDlgItem(pageHost_, kFavoriteClipButtonId), enabled);
        EnableWindow(GetDlgItem(pageHost_, kRenameClipButtonId), enabled);
    }
    if (renameEdit_) {
        EnableWindow(renameEdit_, enabled);
    }
}

void MainWindow::onClipSelectionChanged() {
    if (clipList_ && libraryViewMode_ == LibraryViewMode::List) {
        const auto selected = SendMessageW(clipList_, LB_GETCURSEL, 0, 0);
        if (selected != LB_ERR && selected >= 0) {
            selectedClipIndex_ = clipIndexFromListItem(static_cast<UINT>(selected));
        }
    } else if (clipList_ && libraryViewMode_ == LibraryViewMode::Gallery) {
        if (suppressGalleryFollow_) {
            // Gap click moved the listbox cursor without touching a tile:
            // put the cursor back instead of selecting the row's first tile.
            // LB_SETCURSEL sends no notification, so this cannot recurse.
            suppressGalleryFollow_ = false;
            if (gapPrevRow_ != LB_ERR) {
                SendMessageW(clipList_, LB_SETCURSEL, static_cast<WPARAM>(gapPrevRow_), 0);
            }
            return;
        }
        // Gallery rows hold 3 clips but the listbox selects per row. Keyboard
        // navigation changes the row without touching the cursor, so follow it
        // to the row's first tile; mouse clicks already set the exact tile in
        // armClipDrag before this runs.
        const auto selected = SendMessageW(clipList_, LB_GETCURSEL, 0, 0);
        if (selected != LB_ERR && selected >= 0) {
            const size_t rowFirst = static_cast<size_t>(selected) * kGalleryClipColumns;
            const size_t selectedRow = selectedClipIndex_ < clips_.size()
                ? selectedClipIndex_ / kGalleryClipColumns
                : static_cast<size_t>(-1);
            if (rowFirst < clips_.size() && selectedRow != static_cast<size_t>(selected)) {
                const size_t previousIndex = selectedClipIndex_;
                selectedClipIndex_ = rowFirst;
                invalidateClipRow(previousIndex);
                invalidateClipRow(selectedClipIndex_);
            } else if (selectedClipIndex_ >= clips_.size() && rowFirst < clips_.size()) {
                selectedClipIndex_ = rowFirst;
                invalidateClipRow(selectedClipIndex_);
            }
        }
    } else if (clipList_ && selectedClipIndex_ >= clips_.size()) {
        const auto selected = SendMessageW(clipList_, LB_GETCURSEL, 0, 0);
        if (selected != LB_ERR && selected >= 0) {
            selectedClipIndex_ = clipIndexFromListItem(static_cast<UINT>(selected));
        }
    }
    const auto path = selectedClipPath();
    if (!path.empty()) {
        setText(renameEdit_, path.stem().wstring());
    } else {
        setText(renameEdit_, L"");
    }
    updateLibraryActionButtons();
}

void MainWindow::openSelectedClip() {
    const auto path = selectedClipPath();
    if (path.empty()) {
        setStatus(L"Select a clip to open");
        return;
    }

    if (shellExecuteSucceeded(ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL))) {
        Logger::instance().info(L"ui", std::wstring(L"Opened clip from library: ") + path.wstring());
        setStatus(L"Opened " + path.filename().wstring());
    } else {
        Logger::instance().warning(L"ui", std::wstring(L"Shell could not open clip from library: ") + path.wstring());
        setStatus(L"Clip could not be opened");
    }
}

size_t MainWindow::clipIndexForPath(const std::filesystem::path& path) const {
    const auto found = std::find_if(clips_.begin(), clips_.end(), [&path](const ClipInfo& clip) {
        return clip.path == path;
    });
    // Not found yields clips_.size(), which every caller treats as "no entry".
    return static_cast<size_t>(found - clips_.begin());
}

void MainWindow::deleteSelectedClip() {
    const auto path = selectedClipPath();
    if (path.empty()) {
        setStatus(L"Select a clip to delete");
        return;
    }

    const std::wstring message =
        L"Delete " + path.filename().wstring() + L"?\n\nThis removes the clip file from disk.";
    if (MessageBoxW(window_, message.c_str(), L"Delete Clip", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        setStatus(L"Delete canceled");
        return;
    }

    if (clipManager_.removeClip(path)) {
        Logger::instance().info(L"ui", std::wstring(L"Deleted clip from library: ") + path.wstring());
        // Drop the entry instead of rescanning: a full rescan is a folder walk
        // plus a shell property read per clip, which is what makes a
        // single-clip action freeze the window.
        const size_t index = clipIndexForPath(path);
        if (index < clips_.size()) {
            clips_.erase(clips_.begin() + static_cast<std::ptrdiff_t>(index));
            if (index < clipThumbnails_.size()) {
                clipThumbnails_.erase(clipThumbnails_.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }
        selectedClipIndex_ = static_cast<size_t>(-1);
        // Releases the deleted clip's cached bitmaps without discarding
        // in-flight decodes for the clips that remain.
        refreshThumbnailsAfterLibraryEdit();
        populateClipListRows();
        setText(renameEdit_, L"");
        setStatus(L"Clip deleted");
    } else {
        Logger::instance().warning(L"ui", std::wstring(L"Clip delete failed from library: ") + path.wstring());
        setStatus(L"Clip could not be deleted");
    }
}

void MainWindow::renameSelectedClip() {
    const auto path = selectedClipPath();
    const auto newName = trimText(readText(renameEdit_));
    if (path.empty()) {
        setStatus(L"Select a clip to rename");
        return;
    }
    if (newName.empty()) {
        setStatus(L"Clip name cannot be empty");
        return;
    }

    if (clipManager_.renameClip(path, newName)) {
        Logger::instance().info(L"ui", std::wstring(L"Renamed clip from library: ") + path.wstring() + L" -> " + newName);
        // Same rule as delete: patch the loaded entry instead of rescanning. A
        // rename touches neither the media nor its sidecars, so size, duration,
        // favorite and tags on the entry are still true and must be carried
        // over rather than re-read.
        const size_t index = clipIndexForPath(path);
        if (index < clips_.size()) {
            const std::filesystem::path renamed =
                path.parent_path() / (newName + path.extension().wstring());
            clips_[index].path = renamed;
            // A rename preserves the file's modified time but changes its name,
            // and the list orders by modified time then filename. Without this
            // re-sort a clip that ties on modified time would sit wherever the
            // old name left it instead of where listClips() puts it. This
            // mirrors the comparator there and must stay in step with it.
            std::stable_sort(clips_.begin(), clips_.end(), [](const ClipInfo& left, const ClipInfo& right) {
                if (left.modifiedTime != right.modifiedTime) {
                    return left.modifiedTime > right.modifiedTime;
                }
                return left.path.filename().wstring() > right.path.filename().wstring();
            });
            selectedClipIndex_ = clipIndexForPath(renamed);
            // Drops the thumbnail cached under the old path without
            // discarding in-flight decodes for the clips that remain.
            refreshThumbnailsAfterLibraryEdit();
            populateClipListRows();
        }
        setStatus(L"Clip renamed");
    } else {
        Logger::instance().warning(L"ui", std::wstring(L"Clip rename failed from library: ") + path.wstring() + L" -> " + newName);
        setStatus(L"Clip could not be renamed");
    }
}

void MainWindow::toggleSelectedFavorite() {
    const auto path = selectedClipPath();
    if (path.empty()) {
        setStatus(L"Select a clip to favorite");
        return;
    }
    std::error_code favoriteError;
    const bool currentlyFavorite =
        std::filesystem::exists(path.parent_path() / (path.filename().wstring() + L".favorite"), favoriteError) &&
        !favoriteError;
    if (clipManager_.setFavorite(path, !currentlyFavorite)) {
        Logger::instance().info(L"ui", std::wstring(currentlyFavorite ? L"Removed favorite marker from clip: " : L"Marked clip as favorite: ") + path.wstring());
        // The marker file is the only thing that changed: the clip's own name,
        // size and modified time are untouched, so the loaded list is already
        // in the order a rescan would produce. Flip the entry and repaint.
        const size_t index = clipIndexForPath(path);
        if (index < clips_.size()) {
            clips_[index].favorite = !currentlyFavorite;
            // Favorite is paint state, not a sort key: repaint the row instead
            // of rebuilding the whole list (no flicker, no scroll jump).
            invalidateClipRow(index);
        }
        setStatus(currentlyFavorite ? L"Favorite removed" : L"Clip favorited");
    } else {
        Logger::instance().warning(L"ui", std::wstring(L"Favorite update failed for clip: ") + path.wstring());
        setStatus(L"Favorite could not be updated");
    }
}

std::filesystem::path MainWindow::selectedClipPath() const {
    if (!clipList_) {
        return {};
    }
    if (selectedClipIndex_ < clips_.size()) {
        return clips_[selectedClipIndex_].path;
    }
    const auto selected = SendMessageW(clipList_, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR || selected < 0) {
        return {};
    }
    const size_t index = clipIndexFromListItem(static_cast<UINT>(selected));
    if (index >= clips_.size()) {
        return {};
    }
    return clips_[index].path;
}

} // namespace backtrack
