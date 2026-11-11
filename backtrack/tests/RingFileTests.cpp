#include "replay/RingFile.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace backtrack {

struct RingFileTestAccess {
    template <typename Io>
    static bool write(RingFile& ring, const uint8_t* data, uint32_t size, RingExtent& first,
                      RingExtent& second, const Io& io, DWORD timeout) {
        return ring.writeWrappedImpl(data, size, first, second, L"ring", io, timeout);
    }

    template <typename Io>
    static bool read(RingFile& ring, uint64_t offset, uint8_t* data, uint32_t size,
                     const Io& io, DWORD timeout) {
        return RingFile::transfer(ring.file_, false, offset, data, size, io, timeout);
    }

    static bool readHandle(HANDLE handle, uint8_t* data, uint32_t size, DWORD timeout) {
        return RingFile::transfer(handle, false, 0, data, size, RingFile::Win32Io{}, timeout);
    }
};

} // namespace backtrack

namespace {

using namespace backtrack;
using namespace std::chrono_literals;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct TestRoot {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        (L"backtrack_ring_io_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    TestRoot() {
        std::error_code error;
        expect(std::filesystem::create_directory(path, error) && !error, "could not create test directory");
    }
    ~TestRoot() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct FakeState {
    HANDLE file = nullptr;
    OVERLAPPED* overlapped = nullptr;
    uint8_t* buffer = nullptr;
    DWORD size = 0;
    std::atomic<bool> submitted{false};
    std::atomic<bool> completed{false};
    std::atomic<unsigned> resultCalls{0};
    std::atomic<unsigned> cancellations{0};
    DWORD completionError = ERROR_SUCCESS;
    std::optional<DWORD> completedBytes;
    DWORD immediateError = ERROR_SUCCESS;
    bool signalOnSubmit = false;
};

struct FakeIo {
    std::shared_ptr<FakeState> state;
    std::shared_ptr<int> lease;

    BOOL submit(HANDLE file, bool, void* data, DWORD size, DWORD*, OVERLAPPED* overlapped) const {
        state->file = file;
        state->overlapped = overlapped;
        state->buffer = static_cast<uint8_t*>(data);
        state->size = size;
        state->submitted.store(true, std::memory_order_release);
        if (state->immediateError != ERROR_SUCCESS) {
            SetLastError(state->immediateError);
            return FALSE;
        }
        if (state->signalOnSubmit) {
            std::memset(data, 0x52, size);
            state->completed.store(true, std::memory_order_release);
            SetEvent(overlapped->hEvent);
        }
        SetLastError(ERROR_IO_PENDING);
        return FALSE;
    }

    BOOL result(HANDLE, OVERLAPPED*, DWORD* moved) const {
        state->resultCalls.fetch_add(1, std::memory_order_relaxed);
        if (!state->completed.load(std::memory_order_acquire)) {
            SetLastError(ERROR_IO_INCOMPLETE);
            return FALSE;
        }
        if (state->completionError != ERROR_SUCCESS) {
            SetLastError(state->completionError);
            return FALSE;
        }
        *moved = state->completedBytes.value_or(state->size);
        return TRUE;
    }

    void cancel(HANDLE, OVERLAPPED*) const {
        state->cancellations.fetch_add(1, std::memory_order_relaxed);
        SetLastError(ERROR_NOT_FOUND);
    }
};

struct PartialIo {
    std::shared_ptr<std::array<uint64_t, 3>> offsets;
    std::shared_ptr<unsigned> calls;

    BOOL submit(HANDLE, bool, void* data, DWORD size, DWORD* moved, OVERLAPPED* overlapped) const {
        const uint64_t offset = (static_cast<uint64_t>(overlapped->OffsetHigh) << 32) | overlapped->Offset;
        if (*calls < offsets->size()) (*offsets)[*calls] = offset;
        ++*calls;
        *moved = (std::min<DWORD>)(size, 2);
        for (DWORD i = 0; i < *moved; ++i) static_cast<uint8_t*>(data)[i] = static_cast<uint8_t>(offset + i);
        return TRUE;
    }
    BOOL result(HANDLE, OVERLAPPED*, DWORD*) const { return FALSE; }
    void cancel(HANDLE, OVERLAPPED*) const {}
};

template <typename Predicate>
bool waitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    return predicate();
}

void complete(FakeState& state) {
    state.completed.store(true, std::memory_order_release);
    SetEvent(state.overlapped->hEvent);
}

void actualWrite(HANDLE file, uint64_t offset, const uint8_t* data, DWORD size) {
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    expect(overlapped.hEvent != nullptr, "fixture event creation failed");
    DWORD moved = 0;
    BOOL ok = WriteFile(file, data, size, &moved, &overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(file, &overlapped, &moved, TRUE);
    }
    CloseHandle(overlapped.hEvent);
    expect(ok && moved == size, "fixture late write failed");
}

void testWrapAndRoundTrip() {
    TestRoot root;
    RingFile ring;
    expect(ring.open(root.path, L"ring.bin", 8), "open failed");
    RingExtent first, second;
    const std::array<uint8_t, 6> original{1, 2, 3, 4, 5, 6};
    expect(ring.writeWrapped(original.data(), 6, first, second, L"ring"), "initial write failed");
    ring.releaseBytes(6);
    const std::array<uint8_t, 4> wrapped{7, 8, 9, 10};
    expect(ring.writeWrapped(wrapped.data(), 4, first, second, L"ring"), "wrapped write failed");
    expect(first.offset == 6 && first.byteCount == 2 && second.offset == 0 && second.byteCount == 2,
           "wrapped extents changed");
    std::array<uint8_t, 4> output{};
    expect(ring.read(first.offset, output.data(), first.byteCount) &&
           ring.read(second.offset, output.data() + first.byteCount, second.byteCount), "wrapped read failed");
    expect(output == wrapped, "wrapped bytes changed");
    ring.closeNow();
}

void testCompletedAndImmediateErrors() {
    TestRoot root;
    RingFile ring;
    expect(ring.open(root.path, L"ring.bin", 16), "open failed");
    auto state = std::make_shared<FakeState>();
    FakeIo io{state, std::make_shared<int>(1)};
    state->signalOnSubmit = true;
    std::array<uint8_t, 4> output{};
    expect(RingFileTestAccess::read(ring, 0, output.data(), 4, io, 100), "completed pending read failed");
    expect(output == std::array<uint8_t, 4>{0x52, 0x52, 0x52, 0x52}, "completed read was not copied");
    state->completionError = ERROR_CRC;
    expect(!RingFileTestAccess::read(ring, 0, output.data(), 4, io, 100) && GetLastError() == ERROR_READ_FAULT,
           "completed read error changed");
    state->signalOnSubmit = false;
    state->immediateError = ERROR_DISK_FULL;
    RingExtent first, second;
    expect(!RingFileTestAccess::write(ring, output.data(), 4, first, second, io, 100) &&
           GetLastError() == ERROR_DISK_FULL, "immediate write error was lost");
    expect(!ring.writeWrapped(output.data(), 4, first, second, L"ring") && GetLastError() == ERROR_WRITE_FAULT,
           "failed ring accepted another write");
    ring.closeNow();
}

void testCompletedTransferFaults() {
    TestRoot root;
    for (const bool write : {false, true}) {
        for (const DWORD moved : {0u, 5u, 4u}) {
            RingFile ring;
            expect(ring.open(root.path, L"ring.bin", 16), "open failed");
            auto state = std::make_shared<FakeState>();
            state->signalOnSubmit = true;
            state->completedBytes = moved;
            state->completionError = moved == 4 ? ERROR_CRC : ERROR_SUCCESS;
            FakeIo io{state, std::make_shared<int>(1)};
            std::array<uint8_t, 4> bytes{1, 2, 3, 4};
            RingExtent first{7, 8}, second{9, 10};
            const bool ok = write
                ? RingFileTestAccess::write(ring, bytes.data(), 4, first, second, io, 100)
                : RingFileTestAccess::read(ring, 0, bytes.data(), 4, io, 100);
            expect(!ok && GetLastError() == (write ? ERROR_WRITE_FAULT : ERROR_READ_FAULT),
                   "completed transfer did not report the direction-specific fault");
            expect(state->cancellations.load() == 0, "completed transfer was cancelled");
            expect(bytes == std::array<uint8_t, 4>{1, 2, 3, 4}, "failed transfer changed caller bytes");
            expect(ring.liveBytes() == 0 && first.offset == 7 && first.byteCount == 8 &&
                   second.offset == 9 && second.byteCount == 10, "failed transfer published extents");
            ring.closeNow();
        }
    }
}

void testPartialRead() {
    TestRoot root;
    RingFile ring;
    expect(ring.open(root.path, L"ring.bin", 16), "open failed");
    auto offsets = std::make_shared<std::array<uint64_t, 3>>();
    auto calls = std::make_shared<unsigned>(0);
    std::array<uint8_t, 5> output{};
    expect(RingFileTestAccess::read(ring, 3, output.data(), 5, PartialIo{offsets, calls}, 100),
           "partial read failed");
    expect(*calls == 3 && *offsets == std::array<uint64_t, 3>{3, 5, 7} &&
           output == std::array<uint8_t, 5>{3, 4, 5, 6, 7}, "partial read offsets or bytes changed");
    ring.closeNow();
}

void testPendingWriteSurvivesStopAndCannotOverwrite() {
    TestRoot root;
    RingFile oldRing;
    expect(oldRing.open(root.path, L"ring.bin", 16), "open failed");
    RingExtent first, second;
    const std::array<uint8_t, 4> original{1, 2, 3, 4};
    expect(oldRing.writeWrapped(original.data(), 4, first, second, L"ring"), "initial write failed");

    auto state = std::make_shared<FakeState>();
    FakeIo io{state, std::make_shared<int>(1)};
    std::weak_ptr<int> lease = io.lease;
    std::array<uint8_t, 4> input{5, 6, 7, 8};
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    bool result = true;
    DWORD error = ERROR_SUCCESS;
    const auto start = std::chrono::steady_clock::now();
    std::thread worker([&] {
        result = RingFileTestAccess::write(oldRing, input.data(), 4, first, second, io, 30);
        error = GetLastError();
        std::scoped_lock lock(mutex);
        done = true;
        changed.notify_one();
    });
    bool returnedBeforeCompletion = false;
    {
        std::unique_lock lock(mutex);
        returnedBeforeCompletion = changed.wait_for(lock, 1500ms, [&] { return done; });
    }
    if (!returnedBeforeCompletion && state->submitted.load(std::memory_order_acquire)) complete(*state);
    worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    expect(returnedBeforeCompletion && !result && error == ERROR_WRITE_FAULT && elapsed >= 10ms && elapsed < 1500ms,
           "pending write did not return on deadline");
    expect(state->cancellations.load() == 1, "pending write was not cancelled");
    io.lease.reset();
    expect(!lease.expired(), "pending operation lost its owned storage");
    input.fill(0xff);
    expect(std::memcmp(state->buffer, std::array<uint8_t, 4>{5, 6, 7, 8}.data(), 4) == 0,
           "pending write retained the caller's buffer");
    expect(!oldRing.writeWrapped(input.data(), 4, first, second, L"ring"), "timed-out ring was reused");
    std::array<uint8_t, 4> oldOutput{};
    expect(oldRing.liveBytes() == 4 && oldRing.read(0, oldOutput.data(), 4) && oldOutput == original,
           "failed write changed existing extents");

    oldRing.closeNow();
    DWORD flags = 0;
    expect(GetHandleInformation(state->file, &flags) && GetHandleInformation(state->overlapped->hEvent, &flags),
           "stop closed an outstanding operation's handles");
    RingFile newRing;
    if (!newRing.open(root.path, L"ring.bin", 16)) {
        // A delete-pending old name can be unavailable until the delayed I/O completes.
        expect(newRing.open(root.path, L"replacement.bin", 16), "replacement open failed");
    }
    const std::array<uint8_t, 8> replacement{9, 9, 9, 9, 9, 9, 9, 9};
    expect(newRing.writeWrapped(replacement.data(), 8, first, second, L"ring"), "replacement write failed");

    // An early signal must not release the operation while completion is still pending.
    SetEvent(state->overlapped->hEvent);
    expect(waitFor([&] { return state->resultCalls.load() != 0; }), "completion wait did not run");
    expect(!lease.expired(), "incomplete operation was released");
    const uint64_t offset = (static_cast<uint64_t>(state->overlapped->OffsetHigh) << 32) | state->overlapped->Offset;
    actualWrite(state->file, offset, state->buffer, state->size);
    complete(*state);
    expect(waitFor([&] { return lease.expired(); }), "completed write was not drained");
    std::array<uint8_t, 8> output{};
    expect(newRing.read(0, output.data(), 8) && output == replacement, "late write overwrote the new ring");
    newRing.closeNow();
}

void testRealPendingRead() {
    const std::wstring name = L"\\\\.\\pipe\\backtrack-ring-test-" + std::to_wstring(GetCurrentProcessId());
    HANDLE server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    expect(server != INVALID_HANDLE_VALUE, "pipe creation failed");
    HANDLE client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server);
        throw std::runtime_error("pipe open failed");
    }
    const bool connected = ConnectNamedPipe(server, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
    uint8_t output = 0xcc;
    const auto start = std::chrono::steady_clock::now();
    const bool ok = connected && RingFileTestAccess::readHandle(client, &output, 1, 30);
    const DWORD error = GetLastError();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CloseHandle(client);
    CloseHandle(server);
    expect(connected && !ok && error == ERROR_READ_FAULT && elapsed >= 10ms && elapsed < 1500ms && output == 0xcc,
           "real pending read did not time out safely");
}

void testPendingReadCannotOverwriteCaller() {
    TestRoot root;
    RingFile ring;
    expect(ring.open(root.path, L"ring.bin", 16), "open failed");
    auto state = std::make_shared<FakeState>();
    FakeIo io{state, std::make_shared<int>(1)};
    std::weak_ptr<int> lease = io.lease;
    std::array<uint8_t, 4> output{1, 2, 3, 4};
    expect(!RingFileTestAccess::read(ring, 0, output.data(), 4, io, 30) && GetLastError() == ERROR_READ_FAULT,
           "pending read did not return on deadline");
    io.lease.reset();
    ring.closeNow();
    expect(!lease.expired() && state->cancellations.load() == 1, "pending read lost its storage");
    output.fill(0xcc);
    std::memset(state->buffer, 0x55, state->size);
    state->completionError = ERROR_OPERATION_ABORTED;
    complete(*state);
    expect(waitFor([&] { return lease.expired(); }), "completed read was not drained");
    expect(output == std::array<uint8_t, 4>{0xcc, 0xcc, 0xcc, 0xcc}, "late read overwrote the caller");
}

} // namespace

int main() {
    try {
        testWrapAndRoundTrip();
        testCompletedAndImmediateErrors();
        testCompletedTransferFaults();
        testPartialRead();
        testPendingWriteSurvivesStopAndCannotOverwrite();
        testRealPendingRead();
        testPendingReadCannotOverwriteCaller();
        std::cout << "RingFile tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RingFile test failure: " << error.what() << '\n';
        return 1;
    }
}
