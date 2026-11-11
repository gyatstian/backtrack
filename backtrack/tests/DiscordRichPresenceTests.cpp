#include "integrations/DiscordRichPresence.cpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace backtrack {
std::wstring foregroundApplicationName() { return {}; }
bool foregroundWindowIsFullscreen() { return false; }
void setThreadDescriptionSafe(const wchar_t*) {}

struct DiscordRichPresenceTestAccess {
    static void runStoppedWorker(DiscordRichPresence& presence, HANDLE pipe) {
        presence.pipe_ = pipe;
        presence.stopRequested_ = true;
        presence.workerLoop();
    }

    static void startDrain(DiscordRichPresence& presence, HANDLE pipe) {
        presence.pipe_ = pipe;
        presence.stopRequested_ = false;
        presence.worker_ = std::thread([&presence] { presence.drainInbox(); });
    }
};
}

namespace {

using namespace std::chrono_literals;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct PipePair {
    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;

    PipePair() {
        static std::atomic<unsigned> next{0};
        const std::wstring path = L"\\\\.\\pipe\\backtrack-discord-test-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(next++);
        server = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
        if (server == INVALID_HANDLE_VALUE) throw std::runtime_error("create pipe failed");
        client = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (client == INVALID_HANDLE_VALUE) {
            CloseHandle(server);
            throw std::runtime_error("open pipe failed");
        }
        if (!ConnectNamedPipe(server, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(client);
            CloseHandle(server);
            throw std::runtime_error("connect pipe failed");
        }
    }

    ~PipePair() {
        CloseHandle(client);
        CloseHandle(server);
    }

    void send(const void* data, DWORD length) {
        DWORD written = 0;
        expect(WriteFile(server, data, length, &written, nullptr) && written == length, "pipe fixture write failed");
    }
};

void expectCancellable(PipePair& pipe, void* data, DWORD length, bool write) {
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool done = false;
    bool result = false;
    std::thread worker([&] {
        {
            std::scoped_lock lock(mutex);
            started = true;
            changed.notify_one();
        }
        result = backtrack::pipeTransferExact(pipe.client, data, length, write, &stop,
            std::chrono::steady_clock::now() + 30s);
        {
            std::scoped_lock lock(mutex);
            done = true;
            changed.notify_one();
        }
    });

    bool wasPending = false;
    bool cancelled = false;
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return started; });
        wasPending = !changed.wait_for(lock, 150ms, [&] { return done; });
        stop.store(true, std::memory_order_release);
        cancelled = changed.wait_for(lock, 1500ms, [&] { return done; });
    }
    if (!cancelled) DisconnectNamedPipe(pipe.server);
    worker.join();
    expect(wasPending, "fixture did not leave an operation pending");
    expect(cancelled && !result, "pending pipe operation did not cancel promptly");
}

void testPendingReadAndWrite() {
    {
        PipePair pipe;
        char byte = 0;
        expectCancellable(pipe, &byte, 1, false);
    }
    {
        PipePair pipe;
        std::vector<char> payload(1024 * 1024, 'x');
        expectCancellable(pipe, payload.data(), static_cast<DWORD>(payload.size()), true);
    }
}

void testPartialFrame() {
    PipePair pipe;
    const uint32_t header[2] = {1, 16};
    pipe.send(header, sizeof(header));
    const char first = 'x';
    pipe.send(&first, 1);

    uint32_t received[2]{};
    std::atomic<bool> stop{false};
    expect(backtrack::pipeTransferExact(pipe.client, received, sizeof(received), false, &stop,
        std::chrono::steady_clock::now() + 2s), "frame header was not read");
    expect(received[0] == 1 && received[1] == 16, "frame header changed");
    char payload[16]{};
    expectCancellable(pipe, payload, sizeof(payload), false);
}

void testStopJoinsPartialFrame() {
    PipePair pipe;
    const uint32_t header[2] = {1, 16};
    pipe.send(header, sizeof(header));
    const char first = 'x';
    pipe.send(&first, 1);

    backtrack::DiscordRichPresence presence;
    const HANDLE client = pipe.client;
    backtrack::DiscordRichPresenceTestAccess::startDrain(presence, client);
    pipe.client = INVALID_HANDLE_VALUE;
    DWORD available = 0;
    bool consumed = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        if (!PeekNamedPipe(client, nullptr, 0, nullptr, &available, nullptr)) break;
        if (!available) {
            consumed = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);

    std::mutex mutex;
    std::condition_variable changed;
    bool stopped = false;
    std::thread stopper([&] {
        presence.stop();
        std::scoped_lock lock(mutex);
        stopped = true;
        changed.notify_one();
    });
    bool joined = false;
    {
        std::unique_lock lock(mutex);
        joined = changed.wait_for(lock, 1500ms, [&] { return stopped; });
    }
    if (!joined) DisconnectNamedPipe(pipe.server);
    stopper.join();
    expect(consumed, "partial frame was not consumed");
    expect(joined, "stop did not join the partial-frame reader");
}

void testStoppedWorkerClearsActivity() {
    PipePair pipe;
    backtrack::DiscordRichPresence presence;
    HANDLE client = nullptr;
    expect(DuplicateHandle(GetCurrentProcess(), pipe.client, GetCurrentProcess(), &client,
        0, FALSE, DUPLICATE_SAME_ACCESS), "could not duplicate exit-clear pipe");
    const auto start = std::chrono::steady_clock::now();
    backtrack::DiscordRichPresenceTestAccess::runStoppedWorker(presence, client);
    expect(std::chrono::steady_clock::now() - start < 1500ms, "exit clear exceeded its shutdown budget");

    DWORD available = 0;
    expect(PeekNamedPipe(pipe.server, nullptr, 0, nullptr, &available, nullptr) && available >= 8,
        "stopped worker did not send an exit clear");
    uint32_t header[2]{};
    DWORD read = 0;
    expect(ReadFile(pipe.server, header, sizeof(header), &read, nullptr) && read == sizeof(header),
        "exit clear header was not readable");
    expect(header[0] == backtrack::kOpcodeFrame && header[1] == available - sizeof(header),
        "exit clear framing changed");
    std::string payload(header[1], '\0');
    expect(ReadFile(pipe.server, payload.data(), header[1], &read, nullptr) && read == header[1],
        "exit clear payload was not readable");
    const std::string prefix = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
        std::to_string(GetCurrentProcessId()) + ",\"activity\":null},\"nonce\":\"backtrack-";
    expect(payload.starts_with(prefix) && payload.ends_with("\"}"), "exit clear payload changed");
}

void testCompletionOwnsPendingStorage() {
    auto operation = std::make_shared<backtrack::PipeOperation>();
    operation->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    expect(operation->overlapped.hEvent != nullptr, "event creation failed");
    operation->buffer = std::make_shared<std::vector<uint8_t>>(16);
    auto weak = std::weak_ptr<backtrack::PipeOperation>(operation);
    auto weakBuffer = std::weak_ptr<std::vector<uint8_t>>(operation->buffer);
    HANDLE signalEvent = nullptr;
    expect(DuplicateHandle(GetCurrentProcess(), operation->overlapped.hEvent, GetCurrentProcess(), &signalEvent,
        0, FALSE, DUPLICATE_SAME_ACCESS), "event duplication failed");
    auto owner = std::make_unique<std::shared_ptr<backtrack::PipeOperation>>(operation);
    PTP_WAIT wait = CreateThreadpoolWait(backtrack::releasePipeOperation, owner.get(), nullptr);
    if (!wait) {
        CloseHandle(signalEvent);
        throw std::runtime_error("completion wait creation failed");
    }
    SetThreadpoolWait(wait, operation->overlapped.hEvent, nullptr);
    owner.release();
    operation.reset();
    const bool retained = !weak.expired() && !weakBuffer.expired();
    SetEvent(signalEvent);
    CloseHandle(signalEvent);
    expect(retained, "pending storage was released before completion");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!weak.expired() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
    expect(weak.expired() && weakBuffer.expired(), "completed storage was not released");
}

} // namespace

int main() {
    try {
        testPendingReadAndWrite();
        testPartialFrame();
        testStopJoinsPartialFrame();
        testStoppedWorkerClearsActivity();
        testCompletionOwnsPendingStorage();
        std::cout << "DiscordRichPresenceTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DiscordRichPresenceTests failed: " << error.what() << '\n';
        return 1;
    }
}
