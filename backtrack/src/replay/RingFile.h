#pragma once

#include "core/Logger.h"

#include <Windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
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
// next keyframe so the buffer stays decodable, audio evicts in insertion
// order), passed into ensureSpace as a callback, because the queues
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
        // Never truncate an ambiguous legacy ring or a game's file; the OS
        // removes only the file created by this handle, including after a crash.
        const std::filesystem::path preferred = directory / fileName;
        for (uint32_t suffix = 0; suffix < 10000; ++suffix) {
            const auto candidate = suffix == 0 ? preferred :
                directory / (preferred.stem().wstring() + L"_" + std::to_wstring(suffix) + preferred.extension().wstring());
            const HANDLE handle = CreateFileW(
                candidate.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                file_ = handle;
                path_ = candidate;
                break;
            }
            const DWORD win32Error = GetLastError();
            if (win32Error != ERROR_FILE_EXISTS && win32Error != ERROR_ALREADY_EXISTS) {
                Logger::instance().warning(L"replay",
                    L"Could not create replay ring file " + candidate.wstring() +
                    L" (Win32 error " + std::to_wstring(win32Error) + L")");
                return false;
            }
        }
        if (!file_) {
            Logger::instance().warning(L"replay", L"Could not find a free replay ring name in " + directory.wstring());
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
        writeFailed_ = false;
        return true;
    }

    void closeNow() noexcept {
        if (file_) {
            CloseHandle(file_);
            file_ = nullptr;
        }
        path_.clear();
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

    // Only a physical prefix may be released: freeBytes() assumes all live
    // extents form one contiguous span ending at the write cursor.
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
        return transfer(file_, false, offset, destination, size, Win32Io{}, kRingIoTimeoutMs);
    }

    // Wrap-split write at the cursor; records the one or two extents and
    // advances the cursor. Caller must have secured space via ensureSpace.
    // False on any write failure - the caller must then stop trusting the
    // ring contents. `what` names the track in the log ("ring" vs
    // "audio ring").
    bool writeWrapped(const uint8_t* data, uint32_t size, RingExtent& first, RingExtent& second, const wchar_t* what) {
        return writeWrappedImpl(data, size, first, second, what, Win32Io{}, kRingIoTimeoutMs);
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
    friend struct RingFileTestAccess;

    static constexpr DWORD kRingIoTimeoutMs = 8000;

    struct Win32Io {
        BOOL submit(HANDLE file, bool write, void* data, DWORD size, DWORD* moved, OVERLAPPED* overlapped) const {
            const BOOL started = write ? WriteFile(file, data, size, nullptr, overlapped)
                                       : ReadFile(file, data, size, nullptr, overlapped);
            return started ? GetOverlappedResult(file, overlapped, moved, FALSE) : FALSE;
        }
        BOOL result(HANDLE file, OVERLAPPED* overlapped, DWORD* moved) const {
            return GetOverlappedResult(file, overlapped, moved, FALSE);
        }
        void cancel(HANDLE file, OVERLAPPED* overlapped) const {
            CancelIoEx(file, overlapped);
        }
    };

    template <typename Io>
    struct Operation {
        explicit Operation(const Io& api) : io(api) {}
        ~Operation() {
            if (file) CloseHandle(file);
            if (overlapped.hEvent) CloseHandle(overlapped.hEvent);
        }
        HANDLE file = nullptr;
        OVERLAPPED overlapped{};
        std::unique_ptr<uint8_t[]> buffer;
        Io io;
        SRWLOCK drainLock = SRWLOCK_INIT;
    };

    template <typename Io>
    static void CALLBACK drain(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT wait, TP_WAIT_RESULT) noexcept {
        auto* owner = static_cast<std::shared_ptr<Operation<Io>>*>(context);
        auto operation = *owner;
        AcquireSRWLockExclusive(&operation->drainLock);
        DWORD moved = 0;
        if (!operation->io.result(operation->file, &operation->overlapped, &moved) &&
            GetLastError() == ERROR_IO_INCOMPLETE) {
            // Recheck after reset so a completion racing the signal is not lost.
            ResetEvent(operation->overlapped.hEvent);
            if (!operation->io.result(operation->file, &operation->overlapped, &moved) &&
                GetLastError() == ERROR_IO_INCOMPLETE) {
                SetThreadpoolWait(wait, operation->overlapped.hEvent, nullptr);
                ReleaseSRWLockExclusive(&operation->drainLock);
                return;
            }
        }
        CloseThreadpoolWait(wait);
        delete owner;
        ReleaseSRWLockExclusive(&operation->drainLock);
    }

    template <typename Io>
    static bool transferChunk(HANDLE file, bool write, uint64_t offset, uint8_t* data, DWORD size,
                              const Io& io, DWORD timeout, DWORD& moved, DWORD& error) {
        std::shared_ptr<Operation<Io>> operation;
        try {
            operation = std::make_shared<Operation<Io>>(io);
        } catch (const std::bad_alloc&) {
            error = ERROR_NOT_ENOUGH_MEMORY;
            return false;
        }
        operation->buffer.reset(new (std::nothrow) uint8_t[size]);
        if (!operation->buffer) {
            error = ERROR_NOT_ENOUGH_MEMORY;
            return false;
        }
        if (!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &operation->file,
                             0, FALSE, DUPLICATE_SAME_ACCESS)) {
            error = GetLastError();
            return false;
        }
        operation->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation->overlapped.hEvent) {
            error = GetLastError();
            return false;
        }
        operation->overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
        operation->overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        if (write) std::memcpy(operation->buffer.get(), data, size);

        auto owner = std::unique_ptr<std::shared_ptr<Operation<Io>>>(
            new (std::nothrow) std::shared_ptr<Operation<Io>>(operation));
        if (!owner) {
            error = ERROR_NOT_ENOUGH_MEMORY;
            return false;
        }
        PTP_WAIT completion = CreateThreadpoolWait(drain<Io>, owner.get(), nullptr);
        if (!completion) {
            error = GetLastError();
            return false;
        }
        const ULONGLONG deadline = GetTickCount64() + timeout;
        const BOOL started = io.submit(operation->file, write, operation->buffer.get(), size,
                                       &moved, &operation->overlapped);
        if (!started) {
            error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                CloseThreadpoolWait(completion);
                return false;
            }
            const ULONGLONG now = GetTickCount64();
            const DWORD remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
            const DWORD waitResult = WaitForSingleObject(operation->overlapped.hEvent, remaining);
            bool completed = false;
            if (waitResult == WAIT_OBJECT_0) {
                completed = io.result(operation->file, &operation->overlapped, &moved) != FALSE;
                if (!completed && GetLastError() != ERROR_IO_INCOMPLETE) {
                    CloseThreadpoolWait(completion);
                    error = write ? ERROR_WRITE_FAULT : ERROR_READ_FAULT;
                    return false;
                }
            }
            if (!completed) {
                // Cancellation is only a request; the wait retains every kernel-visible object.
                io.cancel(operation->file, &operation->overlapped);
                owner.release();
                SetThreadpoolWait(completion, operation->overlapped.hEvent, nullptr);
                error = write ? ERROR_WRITE_FAULT : ERROR_READ_FAULT;
                return false;
            }
        }
        CloseThreadpoolWait(completion);
        if (moved == 0 || moved > size) {
            error = write ? ERROR_WRITE_FAULT : ERROR_READ_FAULT;
            return false;
        }
        if (!write) std::memcpy(data, operation->buffer.get(), moved);
        return true;
    }

    template <typename Io>
    static bool transfer(HANDLE file, bool write, uint64_t offset, uint8_t* data, uint32_t size,
                         const Io& io, DWORD timeout) {
        uint32_t done = 0;
        while (done < size) {
            const DWORD chunk = static_cast<DWORD>((std::min<uint64_t>)(size - done, 16ull * 1024 * 1024));
            DWORD moved = 0;
            DWORD error = ERROR_SUCCESS;
            if (!transferChunk(file, write, offset + done, data + done, chunk, io, timeout, moved, error)) {
                SetLastError(error);
                return false;
            }
            done += moved;
        }
        return true;
    }

    template <typename Io>
    bool writeWrappedImpl(const uint8_t* data, uint32_t size, RingExtent& first, RingExtent& second,
                          const wchar_t* what, const Io& io, DWORD timeout) {
        if (writeFailed_) {
            SetLastError(ERROR_WRITE_FAULT);
            return false;
        }
        const uint64_t firstSize = (std::min<uint64_t>)(size, capacityBytes_ - writeOffset_);
        const uint64_t secondSize = size - firstSize;
        auto writeAt = [&](uint64_t offset, const uint8_t* chunk, DWORD byteCount, const wchar_t* action) {
            if (transfer(file_, true, offset, const_cast<uint8_t*>(chunk), byteCount, io, timeout)) return true;
            const DWORD error = GetLastError();
            writeFailed_ = true;
            Logger::instance().warning(L"replay",
                std::wstring(L"Replay ") + what + action + std::to_wstring(error) + L")");
            SetLastError(error);
            return false;
        };
        if (firstSize > 0 && !writeAt(writeOffset_, data, static_cast<DWORD>(firstSize), L" write failed (Win32 error ")) return false;
        if (secondSize > 0 && !writeAt(0, data + firstSize, static_cast<DWORD>(secondSize), L" wrap write failed (Win32 error ")) return false;
        first = RingExtent{writeOffset_, static_cast<uint32_t>(firstSize)};
        second = RingExtent{0, static_cast<uint32_t>(secondSize)};
        writeOffset_ = (writeOffset_ + size) % capacityBytes_;
        liveBytes_ += size;
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
    bool writeFailed_ = false;
};

} // namespace backtrack
