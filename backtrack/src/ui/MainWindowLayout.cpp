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
void MainWindow::layoutWindow() {
    if (!window_) {
        return;
    }

    RECT client{};
    GetClientRect(window_, &client);
    const int clientWidth = std::max(1, static_cast<int>(client.right - client.left));
    const int clientHeight = std::max(1, static_cast<int>(client.bottom - client.top));
    const int contentWidth = std::max(1, clientWidth - kOuterMargin * 2);
    bool changed = false;

    if (!tabButtons_.empty()) {
        const int totalGap = kTabGap * (static_cast<int>(tabButtons_.size()) - 1);
        const int tabWidth = std::max(1, (contentWidth - totalGap) / static_cast<int>(tabButtons_.size()));
        int x = kOuterMargin;
        for (HWND button : tabButtons_) {
            changed = moveWindowIfChanged(button, x, kTabTop, tabWidth, kTabHeight) || changed;
            x += tabWidth + kTabGap;
        }
    }

    const int statusHeight = statusHeightForWidth(contentWidth);
    const int statusTop = std::max(
        kTabTop + kTabHeight + kPageGap + 40,
        clientHeight - kStatusBottomMargin - statusHeight);
    if (status_) {
        changed = moveWindowIfChanged(status_, kOuterMargin, statusTop, contentWidth, statusHeight) || changed;
    }

    const int pageTop = kTabTop + kTabHeight + kPageGap;
    const int pageBottom = std::max(pageTop + 40, statusTop - kPageGap);
    for (auto& cache : pageCaches_) {
        if (cache.host) {
            changed = moveWindowIfChanged(cache.host, kOuterMargin, pageTop, contentWidth, pageBottom - pageTop) || changed;
        }
    }

    changed = layoutCurrentPage() || changed;
    if (changed) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

bool MainWindow::layoutCurrentPage() {
    if (!pageHost_) {
        return false;
    }

    RECT client{};
    GetClientRect(pageHost_, &client);
    const int viewportWidth = std::max(1, static_cast<int>(client.right - client.left));
    const int viewportHeight = std::max(1, static_cast<int>(client.bottom - client.top));
    const int designPageWidth = static_cast<int>(kPageRect.right - kPageRect.left);

    if (page_ == Page::Library && libraryViewMode_ == LibraryViewMode::Gallery && clipList_) {
        const int contentWidth = std::max(120, viewportWidth - kLayoutPadding * 2);
        const int yTitle = kLayoutPadding;
        const int yToolbar = yTitle + 36;
        const int controlHeight = 28;
        const int buttonWidth = 86;
        const int actionGap = 8;
        const int actionGroupGap = 36;
        const int viewButtonsLeft = kLayoutPadding + contentWidth - buttonWidth * 2 - actionGap;
        const int actionButtonWidth = std::clamp(
            (viewButtonsLeft - actionGroupGap - kLayoutPadding - actionGap * 2) / 3,
            56,
            92);
        const int actionsWidth = actionButtonWidth * 3 + actionGap * 2;
        const int actionsLeft = std::max(kLayoutPadding, viewButtonsLeft - actionGroupGap - actionsWidth);
        const int listTop = yToolbar + 34;
        const int listHeight = std::max(180, viewportHeight - listTop - kLayoutPadding);
        const int listWidth = contentWidth;
        const int galleryItemHeight = galleryItemHeightForWidth(listWidth);
        pageContentHeight_ = listTop + listHeight + kLayoutPadding;
        const int maxScroll = std::max(0, pageContentHeight_ - viewportHeight);
        pageScrollY_ = std::clamp(pageScrollY_, 0, maxScroll);

        bool changed = false;
        bool listChanged = false;
        auto move = [&](HWND control, int x, int y, int width, int height) {
            if (control) {
                changed = moveWindowIfChanged(control, x, y - pageScrollY_, width, height) || changed;
                changed = showWindowIfHidden(control) || changed;
            }
        };
        auto movePageControl = [&](size_t index, int x, int y, int width, int height) {
            if (index < pageControls_.size()) {
                move(pageControls_[index], x, y, width, height);
            }
        };

        movePageControl(0, kLayoutPadding, yTitle, 220, 24);
        move(GetDlgItem(pageHost_, kDeleteClipButtonId), actionsLeft, yTitle - 2, actionButtonWidth, controlHeight);
        move(GetDlgItem(pageHost_, kFavoriteClipButtonId), actionsLeft + (actionButtonWidth + actionGap), yTitle - 2, actionButtonWidth, controlHeight);
        move(GetDlgItem(pageHost_, kRefreshClipsButtonId), actionsLeft + (actionButtonWidth + actionGap) * 2, yTitle - 2, actionButtonWidth, controlHeight);
        move(listViewButton_, viewButtonsLeft, yTitle - 2, buttonWidth, controlHeight);
        move(galleryViewButton_, kLayoutPadding + contentWidth - buttonWidth, yTitle - 2, buttonWidth, controlHeight);

        listChanged = setListItemHeightIfChanged(clipList_, clipListItemHeight_, galleryItemHeight);
        move(clipList_, kLayoutPadding, listTop, listWidth, listHeight);

        SCROLLINFO scroll{};
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        scroll.nMin = 0;
        scroll.nMax = std::max(0, pageContentHeight_ - 1);
        scroll.nPage = static_cast<UINT>(viewportHeight);
        scroll.nPos = pageScrollY_;
        SetScrollInfo(pageHost_, SB_VERT, &scroll, TRUE);
        ShowScrollBar(pageHost_, SB_VERT, pageContentHeight_ > viewportHeight);
        if (listChanged) {
            InvalidateRect(clipList_, nullptr, FALSE);
        }
        if (changed || listChanged) {
            InvalidateRect(pageHost_, nullptr, FALSE);
        }
        return changed || listChanged;
    }

    struct Bounds {
        bool present = false;
        int left = std::numeric_limits<int>::max();
        int top = std::numeric_limits<int>::max();
        int right = std::numeric_limits<int>::min();
        int bottom = std::numeric_limits<int>::min();
    };

    Bounds groups[2];
    auto isSettingsCategoryTab = [this](HWND control) {
        return page_ == Page::Settings &&
               std::find(settingsCategoryButtons_.begin(), settingsCategoryButtons_.end(), control) != settingsCategoryButtons_.end();
    };
    auto groupFor = [this, designPageWidth](const RECT& rect) {
        if (page_ == Page::Capture || page_ == Page::Settings) {
            return 0;
        }
        return static_cast<int>(rect.left) >= designPageWidth / 2 ? 1 : 0;
    };
    auto include = [](Bounds& bounds, const RECT& rect) {
        const int left = static_cast<int>(rect.left);
        const int top = static_cast<int>(rect.top);
        const int right = static_cast<int>(rect.right);
        const int bottom = static_cast<int>(rect.bottom);
        bounds.present = true;
        bounds.left = std::min(bounds.left, left);
        bounds.top = std::min(bounds.top, top);
        bounds.right = std::max(bounds.right, right);
        bounds.bottom = std::max(bounds.bottom, bottom);
    };

    for (const auto& item : layoutItems_) {
        if (!item.control || item.kind == LayoutItem::Kind::Footer) {
            continue;
        }
        if (isSettingsCategoryTab(item.control)) {
            // Tabs reserve their design-space height but never determine content scaling.
            Bounds& bounds = groups[0];
            bounds.present = true;
            bounds.top = std::min(bounds.top, static_cast<int>(item.design.top));
            bounds.bottom = std::max(bounds.bottom, static_cast<int>(item.design.bottom));
            continue;
        }
        include(groups[groupFor(item.design)], item.design);
    }

    const bool twoColumns =
        viewportWidth >= kTwoColumnMinimumWidth &&
        groups[0].present &&
        groups[1].present;

    struct Placement {
        HWND control = nullptr;
        RECT rect{};
        int windowHeight = 0;
    };
    std::vector<Placement> placements;
    int contentBottom = kLayoutPadding;

    auto scaleValue = [](int value, int target, int source) {
        return source > 0 ? MulDiv(value, target, source) : value;
    };
    auto addPlacement = [&](const LayoutItem& item, int x, int y, int width, int height) {
        RECT rect{x, y, x + width, y + height};
        const int windowHeight = item.windowHeight > 0 ? item.windowHeight : height;
        placements.push_back(Placement{item.control, rect, windowHeight});
        contentBottom = std::max(contentBottom, static_cast<int>(rect.bottom));
    };
    auto placeInColumn = [&](const LayoutItem& item, int group, int xOrigin, int columnWidth, int yOffset) {
        const Bounds& bounds = groups[group];
        const int itemLeft = static_cast<int>(item.design.left);
        const int itemTop = static_cast<int>(item.design.top);
        const int itemRight = static_cast<int>(item.design.right);
        const int itemBottom = static_cast<int>(item.design.bottom);
        const int sourceWidth = std::max(1, bounds.right - bounds.left);
        const int x = xOrigin + scaleValue(itemLeft - bounds.left, columnWidth, sourceWidth);
        const int width = std::max(24, scaleValue(itemRight - itemLeft, columnWidth, sourceWidth));
        const int y = itemTop + yOffset;
        const int height = std::max(1, itemBottom - itemTop);
        addPlacement(item, x, y, width, height);
    };

    if (page_ == Page::Settings && !settingsCategoryButtons_.empty()) {
        constexpr int kSettingsCategoryTabGap = 8;
        constexpr int kSettingsCategoryTabHeight = 30;
        const int tabCount = static_cast<int>(settingsCategoryButtons_.size());
        const int totalGap = kSettingsCategoryTabGap * (tabCount - 1);
        const int tabWidth = std::max(1, (viewportWidth - kLayoutPadding * 2 - totalGap) / tabCount);
        int x = kLayoutPadding;
        for (HWND button : settingsCategoryButtons_) {
            if (button) {
                RECT rect{x, kLayoutPadding, x + tabWidth, kLayoutPadding + kSettingsCategoryTabHeight};
                placements.push_back(Placement{button, rect, kSettingsCategoryTabHeight});
                contentBottom = std::max(contentBottom, static_cast<int>(rect.bottom));
            }
            x += tabWidth + kSettingsCategoryTabGap;
        }
    }

    if (twoColumns) {
        const int columnWidth = std::max(80, (viewportWidth - kLayoutPadding * 2 - kLayoutColumnGap) / 2);
        for (const auto& item : layoutItems_) {
            if (!item.control || isSettingsCategoryTab(item.control)) {
                continue;
            }
            const int group = item.kind == LayoutItem::Kind::Footer ? 0 : groupFor(item.design);
            placeInColumn(
                item,
                group,
                kLayoutPadding + group * (columnWidth + kLayoutColumnGap),
                columnWidth,
                0);
        }
    } else {
        const int columnWidth = std::max(80, viewportWidth - kLayoutPadding * 2);
        int nextTop = kLayoutPadding;
        for (int group = 0; group < 2; ++group) {
            if (!groups[group].present) {
                continue;
            }
            const Bounds& bounds = groups[group];
            const int sourceWidth = std::max(1, bounds.right - bounds.left);
            const int sourceHeight = std::max(1, bounds.bottom - bounds.top);
            int groupBottom = nextTop;

            for (const auto& item : layoutItems_) {
                if (!item.control ||
                    item.kind == LayoutItem::Kind::Footer ||
                    isSettingsCategoryTab(item.control) ||
                    groupFor(item.design) != group) {
                    continue;
                }
                const int itemLeft = static_cast<int>(item.design.left);
                const int itemTop = static_cast<int>(item.design.top);
                const int itemRight = static_cast<int>(item.design.right);
                const int itemBottom = static_cast<int>(item.design.bottom);
                const int x = kLayoutPadding + scaleValue(itemLeft - bounds.left, columnWidth, sourceWidth);
                const int width = std::max(24, scaleValue(itemRight - itemLeft, columnWidth, sourceWidth));
                const int y = nextTop + itemTop - bounds.top;
                const int height = std::max(1, itemBottom - itemTop);
                addPlacement(item, x, y, width, height);
                groupBottom = std::max(groupBottom, y + height);
            }
            nextTop += sourceHeight + kLayoutStackGap;
            contentBottom = std::max(contentBottom, groupBottom);
        }

        const int footerTop = contentBottom + 16;
        for (const auto& item : layoutItems_) {
            if (!item.control || isSettingsCategoryTab(item.control) || item.kind != LayoutItem::Kind::Footer) {
                continue;
            }
            const int itemWidth = static_cast<int>(item.design.right - item.design.left);
            const int itemHeight = static_cast<int>(item.design.bottom - item.design.top);
            const int width = std::min(std::max(120, itemWidth), columnWidth);
            const int height = std::max(1, itemHeight);
            addPlacement(item, kLayoutPadding, footerTop, width, height);
        }
    }

    pageContentHeight_ = contentBottom + kLayoutPadding;
    const int maxScroll = std::max(0, pageContentHeight_ - viewportHeight);
    pageScrollY_ = std::clamp(pageScrollY_, 0, maxScroll);

    SCROLLINFO scroll{};
    scroll.cbSize = sizeof(scroll);
    scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0;
    scroll.nMax = std::max(0, pageContentHeight_ - 1);
    scroll.nPage = static_cast<UINT>(viewportHeight);
    scroll.nPos = pageScrollY_;
    SetScrollInfo(pageHost_, SB_VERT, &scroll, TRUE);
    ShowScrollBar(pageHost_, SB_VERT, pageContentHeight_ > viewportHeight);

    bool changed = false;
    for (const auto& placement : placements) {
        changed = moveWindowIfChanged(
            placement.control,
            placement.rect.left,
            placement.rect.top - pageScrollY_,
            placement.rect.right - placement.rect.left,
            placement.windowHeight) || changed;
    }
    if (changed) {
        InvalidateRect(pageHost_, nullptr, FALSE);
    }
    return changed;
}

void MainWindow::scrollPageTo(int position) {
    if (!pageHost_) {
        return;
    }
    RECT client{};
    GetClientRect(pageHost_, &client);
    const int viewportHeight = std::max(1, static_cast<int>(client.bottom - client.top));
    const int maxScroll = std::max(0, pageContentHeight_ - viewportHeight);
    const int clamped = std::clamp(position, 0, maxScroll);
    if (clamped != pageScrollY_) {
        pageScrollY_ = clamped;
        if (layoutCurrentPage()) {
            redrawWindowAndChildren(pageHost_, true);
        }
    }
}

void MainWindow::scrollPageBy(int delta) {
    scrollPageTo(pageScrollY_ + delta);
}

void MainWindow::scrollPageWheel(WPARAM wParam) {
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == 0) {
        return;
    }

    const int delta = static_cast<int>(GET_WHEEL_DELTA_WPARAM(wParam));
    if (lines == WHEEL_PAGESCROLL) {
        pageWheelRemainder_ += delta;
        const int pages = pageWheelRemainder_ / WHEEL_DELTA;
        pageWheelRemainder_ %= WHEEL_DELTA;
        if (pages != 0) {
            RECT client{};
            GetClientRect(pageHost_, &client);
            const int viewportHeight = std::max(1, static_cast<int>(client.bottom - client.top));
            scrollPageBy(-pages * viewportHeight);
        }
        return;
    }

    constexpr int kWheelPixelsPerLine = 16;
    pageWheelRemainder_ += delta * static_cast<int>(lines) * kWheelPixelsPerLine;
    const int pixels = pageWheelRemainder_ / WHEEL_DELTA;
    pageWheelRemainder_ %= WHEEL_DELTA;
    if (pixels != 0) {
        scrollPageBy(-pixels);
    }
}

bool MainWindow::handleClipListMouseWheel(WPARAM wParam) {
    if (!clipList_ || libraryViewMode_ != LibraryViewMode::Gallery) {
        return false;
    }

    clipListWheelRemainder_ += static_cast<int>(GET_WHEEL_DELTA_WPARAM(wParam));
    const int rows = clipListWheelRemainder_ / WHEEL_DELTA;
    clipListWheelRemainder_ %= WHEEL_DELTA;
    if (rows == 0) {
        return true;
    }

    const LRESULT countResult = SendMessageW(clipList_, LB_GETCOUNT, 0, 0);
    const LRESULT topResult = SendMessageW(clipList_, LB_GETTOPINDEX, 0, 0);
    if (countResult == LB_ERR || topResult == LB_ERR || countResult <= 0) {
        return true;
    }

    RECT client{};
    GetClientRect(clipList_, &client);
    const int listHeight = std::max(1, static_cast<int>(client.bottom - client.top));
    const int itemHeight = std::max(1, clipListItemHeight_);
    const int visibleRows = std::max(1, (listHeight + itemHeight - 1) / itemHeight);
    const int maxTop = std::max(0, static_cast<int>(countResult) - visibleRows);
    const int top = static_cast<int>(topResult);
    const int targetTop = std::clamp(top - rows, 0, maxTop);
    if (targetTop != top) {
        SendMessageW(clipList_, LB_SETTOPINDEX, static_cast<WPARAM>(targetTop), 0);
        InvalidateRect(clipList_, nullptr, FALSE);
    }
    return true;
}

} // namespace backtrack
