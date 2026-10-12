#pragma once

#include "core/Logger.h"

#include <Windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace backtrack {

// One contiguous or wrap-split extent of a packet's bytes inside a ring
// file. secondByteCount == 0 means the payload did not wrap.
struct RingExtent {
    uint64_t offset = 0;
    uint32_t byteCount = 0;
};

// Fixed-capacity byte ring shared by the replay video and audio backends.
// Owns only the byte store: handle, path, capacity, write cursor, and live
// byte count. Eviction policy stays with the caller (video realigns to the
// next keyframe so the buffer stays decodable, audio evicts plain
// oldest-first), passed into ensureSpace as a callback, because the queues
// being evicted live in ReplayBuffer. The save pin lives there too: while a
// save holds it, pushes wait instead of evicting, so trimming can never drop
// bytes an in-flight save has not read yet.
class RingFile {
public:
    bool open(const std::filesystem::path& directory, const wchar_t* fileName, uint64_t capacityBytes) {
        capacityBytes_ = capacityBytes;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            Logger::instance().warning(L"replay",
                L"Could not create replay ring folder " + directory.wstring() +
                L" (" + utf8ToWide(error.message()) + L")");
            return false;
        }
        path_ = directory / fileName;
        file_ = CreateFileW(
            path_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            // Overlapped: every transfer names its own offset, so save reads
            // and push writes never share a file position.
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            file_ = nullptr;
            Logger::instance().warning(L"replay",
                L"Could not create replay ring file " + path_.wstring() +
                L" (Win32 error " + std::to_wstring(GetLastError()) + L")");
            return false;
        }
        // Pre-size the file so the steady-state footprint is fixed from the
        // start. NTFS assigns real clusters lazily, so this costs no I/O
        // beyond metadata until packets actually occupy the ranges.
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(capacityBytes);
        if ((!SetFilePointerEx(file_, end, nullptr, FILE_BEGIN) || !SetEndOfFile(file_))) {
            Logger::instance().warning(L"replay",
                L"Could not pre-size replay ring file " + path_.wstring() +
                L" (Win32 error " + std::to_wstring(GetLastError()) + L"); it will grow as packets arrive");
        }
        writeOffset_ = 0;
        liveBytes_ = 0;
        return true;
    }

    void closeNow() noexcept {
        if (file_) {
            CloseHandle(file_);
            file_ = nullptr;
        }
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove(path_, error);
            // The folder is shared with the other track's ring, so only take
            // it when it is actually empty. Only attempted when the ring file
            // itself is gone.
            if (!error) {
                std::error_code directoryError;
                std::filesystem::remove(path_.parent_path(), directoryError);
            }
            path_.clear();
        }
        writeOffset_ = 0;
        liveBytes_ = 0;
    }

    bool isOpen() const {
        return file_ != nullptr;
    }

    const std::filesystem::path& path() const {
        return path_;
    }

    uint64_t capacity() const {
        return capacityBytes_;
    }

    uint64_t liveBytes() const {
        return liveBytes_;
    }

    // Saturating free-space: a VBR overshoot can push liveBytes_ past the
    // capacity; without the clamp the unsigned difference would wrap to ~2^64
    // and defeat every eviction/wait decision.
    uint64_t freeBytes() const {
        return liveBytes_ >= capacityBytes_ ? 0 : capacityBytes_ - liveBytes_;
    }

    void releaseBytes(uint64_t byteCount) {
        liveBytes_ -= byteCount;
    }

    // Drops the accounting but keeps the write cursor where it is: re-zeroing
    // it would point the next push at offset 0, the most likely place for a
    // still-pinned save to have unread bytes.
    void clearLive() {
        liveBytes_ = 0;
    }

    // Explicit-offset overlapped read; safe with or without the caller's mutex
    // held because no per-handle file position exists.
    bool read(uint64_t offset, uint8_t* destination, uint32_t size) const {
        if (!file_ || size == 0) {
            return size == 0;
        }
        return transfer(file_, false, offset, destination, size);
    }

    // Wrap-split write at the cursor; records the one or two extents and
    // advances the cursor. Caller must have secured space via ensureSpace.
    // False on any write failure - the caller must then stop trusting the
    // ring contents. `what` names the track in the log ("ring" vs
    // "audio ring").
    bool writeWrapped(const uint8_t* data, uint32_t size, RingExtent& first, RingExtent& second, const wchar_t* what) {
        const uint64_t firstSize = (std::min<uint64_t>)(size, capacityBytes_ - writeOffset_);
        const uint64_t secondSize = size - firstSize;
        auto writeAt = [this](uint64_t offset, const uint8_t* chunk, DWORD byteCount) {
            return transfer(file_, true, offset, const_cast<uint8_t*>(chunk), byteCount);
        };
        if (firstSize > 0 && !writeAt(writeOffset_, data, static_cast<DWORD>(firstSize))) {
            Logger::instance().warning(L"replay",
                std::wstring(L"Replay ") + what + L" write failed (Win32 error " + std::to_wstring(GetLastError()) + L")");
            return false;
        }
        if (secondSize > 0 && !writeAt(0, data + firstSize, static_cast<DWORD>(secondSize))) {
            Logger::instance().warning(L"replay",
                std::wstring(L"Replay ") + what + L" wrap write failed (Win32 error " + std::to_wstring(GetLastError()) + L")");
            return false;
        }
        first = RingExtent{writeOffset_, static_cast<uint32_t>(firstSize)};
        // secondSize == 0 marks a non-wrapping packet; its offset is unused.
        second = RingExtent{0, static_cast<uint32_t>(secondSize)};
        writeOffset_ = (writeOffset_ + size) % capacityBytes_;
        liveBytes_ += size;
        return true;
    }

    // Waits (while a save pins refs) or evicts oldest packets until size
    // bytes of ring space are free. False when the request can never fit.
    // evictIfUnpinned frees at least enough for size (the caller's per-track
    // policy); isEmpty reports that nothing is left to evict.
    template <typename EvictIfUnpinned, typename IsEmpty>
    bool ensureSpace(
        std::unique_lock<std::mutex>& lock,
        uint32_t size,
        unsigned& pinRefs,
        std::condition_variable& space,
        EvictIfUnpinned&& evictIfUnpinned,
        IsEmpty&& isEmpty,
        const wchar_t* what) {
        if (size > capacityBytes_) {
            Logger::instance().warning(L"replay",
                std::wstring(L"Replay ") + what + L" packet of " + std::to_wstring(size) +
                L" bytes exceeds the entire " + what + L" capacity; dropping it");
            return false;
        }
        if (freeBytes() >= size) {
            return true;
        }
        while (true) {
            if (pinRefs) {
                // A save is still consuming pinned refs; wait for it to finish
                // rather than evicting packets it may not have read yet.
                space.wait(lock);
            } else {
                evictIfUnpinned(size);
            }
            if (freeBytes() >= size) {
                return true;
            }
            if (!pinRefs && isEmpty()) {
                // Everything evicted and still not enough cannot happen
                // (size <= capacity), but bail out defensively instead of spinning.
                Logger::instance().warning(L"replay",
                    std::wstring(L"Replay ") + what + L" could not free enough space; dropping a packet");
                return false;
            }
        }
    }

    // Test-only fault injection. Drops the handle while leaving the caller's
    // metadata intact, so the next read fails exactly as it would for a
    // vanished volume or a media error.
    void breakForTesting() noexcept {
        if (file_) {
            CloseHandle(file_);
            file_ = nullptr;
        }
    }

private:
    // Bound on a single ring IRP. The ring folder is user-chosen, so a USB
    // stick, a network drive or a sleeping disk can leave a request pending
    // forever, and both callers hold the buffer mutex while this runs: an
    // unbounded wait would wedge the encode loop, the UI, and stopPipeline's
    // thread joins, with no way to close the app. Generous enough that a
    // merely slow removable drive is not mistaken for a stall, short enough
    // that a wedged one degrades to memory rather than hanging.
    static constexpr DWORD kRingIoTimeoutMs = 8000;

    // Explicit-offset synchronous-completion transfer on an overlapped ring
    // handle. Returns false with GetLastError() set on failure.
    static bool transfer(HANDLE file, bool write, uint64_t offset, uint8_t* data, uint32_t size) {
        uint32_t done = 0;
        while (done < size) {
            OVERLAPPED overlapped{};
            const uint64_t absolute = offset + done;
            overlapped.Offset = static_cast<DWORD>(absolute & 0xFFFFFFFFu);
            overlapped.OffsetHigh = static_cast<DWORD>(absolute >> 32);
            const DWORD chunkBytes = static_cast<DWORD>((std::min<uint64_t>)(size - done, 16ull * 1024 * 1024));
            DWORD moved = 0;
            const BOOL ok = write
                ? WriteFile(file, data + done, chunkBytes, &moved, &overlapped)
                : ReadFile(file, data + done, chunkBytes, &moved, &overlapped);
            if (!ok) {
                const DWORD error = GetLastError();
                if (error != ERROR_IO_PENDING) {
                    return false;
                }
                if (GetOverlappedResult(file, &overlapped, &moved, kRingIoTimeoutMs)) {
                    // completed within the budget
                } else {
                    // Returning here would leave the IRP writing into this
                    // frame's OVERLAPPED and into the caller's buffer after
                    // both go away, so cancel and then drain instead:
                    // CancelIoEx guarantees the request completes, which makes
                    // this second INFINITE wait return promptly with
                    // ERROR_OPERATION_ABORTED rather than block. Report the
                    // transfer as failed either way, so the caller falls back
                    // to memory instead of using partial bytes.
                    CancelIoEx(file, &overlapped);
                    GetOverlappedResult(file, &overlapped, &moved, TRUE);
                    SetLastError(write ? ERROR_WRITE_FAULT : ERROR_READ_FAULT);
                    return false;
                }
            }
            if (moved == 0) {
                SetLastError(write ? ERROR_WRITE_FAULT : ERROR_READ_FAULT);
                return false;
            }
            done += moved;
        }
        return true;
    }

    HANDLE file_ = nullptr;
    std::filesystem::path path_;
    uint64_t capacityBytes_ = 0;
    // Next write position inside [0, capacity). Payloads crossing the end of
    // the file wrap around to offset 0 and produce a two-extent packet.
    uint64_t writeOffset_ = 0;
    // Total bytes of all live refs; capacity minus this is the free gap the
    // write cursor may consume before it would collide with the oldest ref.
    uint64_t liveBytes_ = 0;
};

} // namespace backtrack
