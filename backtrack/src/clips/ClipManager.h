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

class ClipManager {
public:
    explicit ClipManager(std::filesystem::path directory = {});

    void setDirectory(std::filesystem::path directory);
    std::vector<ClipInfo> listClips(const std::function<bool()>& cancelled = {}) const;
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
