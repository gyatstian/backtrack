#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace backtrack {

struct ClipInfo {
    std::filesystem::path path;
    std::filesystem::file_time_type modifiedTime{};
    uintmax_t bytes = 0;
    uint64_t duration100ns = 0;
    bool favorite = false;
    std::vector<std::wstring> tags;
};

struct ClipFolder {
    std::filesystem::path path;
    std::wstring name;
    // Clips below the folder at any depth; a folder row shows this count.
    size_t clips = 0;
};

// One library level: its immediate folders and the clips directly inside it.
struct ClipContents {
    std::vector<ClipFolder> folders;
    std::vector<ClipInfo> clips;
};

// The target renameClip would use, or empty when the name is invalid. A
// trailing source extension in newStem is dropped.
std::filesystem::path clipRenameTarget(const std::filesystem::path& path, const std::wstring& newStem);
// True when target exists and is not path itself (a case-only rename).
bool clipRenameCollides(const std::filesystem::path& path, const std::filesystem::path& target);

class ClipManager {
public:
    explicit ClipManager(std::filesystem::path directory = {});

    void setDirectory(std::filesystem::path directory);
    std::vector<ClipInfo> listClips(const std::function<bool()>& cancelled = {}) const;
    // The current directory as one library level, for folder-by-folder browsing.
    // Dot-prefixed folders are omitted unless includeHiddenFolders is set.
    ClipContents listContents(const std::function<bool()>& cancelled = {},
                              bool includeHiddenFolders = false) const;
    bool removeClip(const std::filesystem::path& path) const;
    bool renameClip(const std::filesystem::path& path, const std::wstring& newStem) const;
    bool setFavorite(const std::filesystem::path& path, bool favorite) const;
    bool addTag(const std::filesystem::path& path, const std::wstring& tag) const;

private:
    friend struct ClipManagerTestAccess;

    struct DurationCacheEntry {
        uintmax_t bytes = 0;
        std::filesystem::file_time_type modifiedTime{};
        uint64_t duration100ns = 0;
    };

    bool isManagedClipPath(const std::filesystem::path& path) const;
    // Clips directly inside one directory. Subdirectories that are not reparse
    // points are appended to `subdirectories` in the same pass.
    std::vector<ClipInfo> scanDirectory(const std::filesystem::path& directory,
                                        std::vector<std::filesystem::path>* subdirectories,
                                        const std::function<bool()>& cancelled) const;
    bool renameClipImpl(const std::filesystem::path& path, const std::wstring& newStem,
                        void (*beforeMove)(const std::filesystem::path&)) const;
    std::filesystem::path favoriteMarker(const std::filesystem::path& path) const;
    std::filesystem::path tagFile(const std::filesystem::path& path) const;
    std::vector<std::wstring> readTags(const std::filesystem::path& path) const;
    // Shell property reads are slow (per-file COM handler open), so durations
    // are cached per file identity (size + modified time). Calls on one
    // instance must be serialized.
    uint64_t cachedMediaDuration(
        const std::filesystem::path& path,
        uintmax_t bytes,
        std::filesystem::file_time_type modifiedTime) const;

    std::filesystem::path directory_;
    mutable std::unordered_map<std::wstring, DurationCacheEntry> durationCache_;
};

} // namespace backtrack
