#pragma once

namespace backtrack {

struct StudioNavigation {
    enum class Page { Capture, Library, Settings, Diagnostics };
    enum class Category { App, Capture, Video, Audio, Integrations };

    Page page = Page::Capture;
    Category category = Category::App;
};

} // namespace backtrack
