#include "integrations/DiscordRichPresence.h"

#include "core/Logger.h"
#include "platform/Win32Util.h"

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

namespace backtrack {

namespace {

// Discord application/client id. Register a "Rich Presence" application at
// https://discord.com/developers/applications and paste its Application ID
// here. Without a valid id Discord ignores the activity updates.
constexpr const char* kDiscordApplicationId = "1532158610476630167";

constexpr const char* kGithubButtonLabel = "Backtrack";
constexpr const char* kGithubButtonUrl = "https://github.com/gyatstian/backtrack";

constexpr int kOpcodeHandshake = 0;
constexpr int kOpcodeFrame = 1;
constexpr int kOpcodeClose = 2;

constexpr int kMaxDiscordPipeIndex = 9;
constexpr auto kReconnectInterval = std::chrono::seconds(5);
// Poll cadence for the foreground app, not the send cadence: frames go out
// only when the game changes (or after a reconnect), so a short interval
// costs two local Win32 queries and no extra Discord traffic.
constexpr auto kActivityUpdateInterval = std::chrono::seconds(2);
// Inbound frames are small JSON documents (handshake replies, nonce acks).
// The length field is read raw off the pipe; a corrupt or hostile value must
// not be trusted to size an allocation. Frames larger than this were never
// meaningful, so treat them as a lost connection instead of allocating.
constexpr uint32_t kMaxInboundFrameBytes = 64 * 1024;

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buffer[8]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                out += buffer;
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

std::string nonceString() {
    static std::atomic<uint64_t> counter{1};
    const uint64_t value = counter.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream stream;
    stream << "backtrack-" << value;
    return stream.str();
}

// Byte-mode read: loops until exactly `size` bytes arrive. A single ReadFile
// on a pipe may return fewer bytes than requested, so one call is not enough
// to fill a header or a payload.
bool readExact(HANDLE pipe, void* data, DWORD size) {
    auto* cursor = static_cast<uint8_t*>(data);
    DWORD done = 0;
    while (done < size) {
        DWORD got = 0;
        if (!ReadFile(pipe, cursor + done, size - done, &got, nullptr) || got == 0) {
            return false;
        }
        done += got;
    }
    return true;
}

} // namespace

DiscordRichPresence::~DiscordRichPresence() {
    stop();
}

void DiscordRichPresence::start(GameIntegrationSettings::DiscordRichPresenceMode mode, bool resetTimerOnSwitch) {
    if (mode == GameIntegrationSettings::DiscordRichPresenceMode::Off) {
        // Same gate as VoiceCommandService::configure: never run a worker for
        // Off, and stop one that a settings change left behind.
        stop();
        return;
    }
    std::scoped_lock lock(mutex_);
    mode_.store(mode, std::memory_order_release);
    resetTimerOnSwitch_.store(resetTimerOnSwitch, std::memory_order_release);
    if (worker_.joinable()) {
        return;
    }
    stopRequested_ = false;
    worker_ = std::thread(&DiscordRichPresence::workerLoop, this);
}

void DiscordRichPresence::stop() {
    std::thread worker;
    {
        std::scoped_lock lock(mutex_);
        stopRequested_ = true;
        worker = std::move(worker_);
    }
    if (worker.joinable()) {
        worker.join();
    }
    disconnect();
}

void DiscordRichPresence::workerLoop() {
    setThreadDescriptionSafe(L"Backtrack Discord integration");
    Logger::instance().info(L"discord", L"Discord Rich Presence integration started");

    // An exception leaving a std::thread entry point calls std::terminate, so an
    // internal fault here may only cost Rich Presence, never the app.
    static std::atomic<uint64_t> faultCount{0};

    try {
        std::string lastGameName;
        bool activityShown = false;
        // Session start for the elapsed timer. Set on the first activity send
        // and reused while "keep timer running" is selected, so app switches,
        // fullscreen gaps, and reconnects do not restart it.
        long long activityStartUnixSeconds = 0;
        auto nextActivityUpdate = SteadyClock::time_point::min();
        auto nextReconnect = SteadyClock::now();

        while (!stopRequested_.load(std::memory_order_acquire)) {
            if (!pipe_) {
                if (SteadyClock::now() < nextReconnect) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                if (!connect() || !handshake()) {
                    disconnect();
                    nextReconnect = SteadyClock::now() + kReconnectInterval;
                    continue;
                }
                Logger::instance().info(L"discord", L"Discord IPC connection established");
                nextActivityUpdate = SteadyClock::time_point::min();
                // Force re-send of activity after a fresh connection.
                activityShown = false;
            }

            if (!drainInbox()) {
                Logger::instance().warning(L"discord", L"Discord IPC connection lost");
                disconnect();
                nextReconnect = SteadyClock::now() + kReconnectInterval;
                continue;
            }

            if (SteadyClock::now() >= nextActivityUpdate) {
                // mode_ can change while the worker is running (settings save).
                const bool fullscreenOnly =
                    mode_.load(std::memory_order_acquire) ==
                    GameIntegrationSettings::DiscordRichPresenceMode::FullscreenOnly;
                const bool fullscreen = !fullscreenOnly || foregroundWindowIsFullscreen();
                if (fullscreen) {
                    const std::wstring wideGame = foregroundApplicationName();
                    const std::string gameName = wideToUtf8(wideGame);
                    if (gameName != lastGameName || !activityShown) {
                        const long long nowUnixSeconds =
                            std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
                        if (resetTimerOnSwitch_.load(std::memory_order_acquire) ||
                            activityStartUnixSeconds == 0) {
                            activityStartUnixSeconds = nowUnixSeconds;
                        }
                        if (!updateActivity(gameName, activityStartUnixSeconds)) {
                            Logger::instance().warning(L"discord", L"Discord activity update failed");
                            disconnect();
                            nextReconnect = SteadyClock::now() + kReconnectInterval;
                            continue;
                        }
                        lastGameName = gameName;
                        activityShown = true;
                        Logger::instance().debug(L"discord",
                            gameName.empty()
                                ? std::wstring(L"Discord activity set: Clipping with Backtrack")
                                : std::wstring(L"Discord activity set: Clipping ") + wideGame);
                    }
                } else if (activityShown) {
                    // Foreground left fullscreen; clear the displayed activity.
                    std::ostringstream payload;
                    payload << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" << GetCurrentProcessId()
                            << ",\"activity\":null},\"nonce\":\"" << nonceString() << "\"}";
                    if (!sendFrame(kOpcodeFrame, payload.str())) {
                        Logger::instance().warning(L"discord", L"Discord activity clear failed");
                        disconnect();
                        nextReconnect = SteadyClock::now() + kReconnectInterval;
                        continue;
                    }
                    activityShown = false;
                    lastGameName.clear();
                    Logger::instance().debug(L"discord", L"Discord activity cleared (foreground not fullscreen)");
                }
                nextActivityUpdate = SteadyClock::now() + kActivityUpdateInterval;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    } catch (const std::exception& error) {
        const uint64_t fault = faultCount.fetch_add(1, std::memory_order_relaxed) + 1;
        Logger::instance().error(L"discord",
            std::wstring(L"Discord Rich Presence disabled by an unexpected error (fault ")
            + std::to_wstring(fault) + L"): " + utf8ToWide(error.what()));
    } catch (...) {
        const uint64_t fault = faultCount.fetch_add(1, std::memory_order_relaxed) + 1;
        Logger::instance().error(L"discord",
            std::wstring(L"Discord Rich Presence disabled by an unknown error (fault ")
            + std::to_wstring(fault) + L")");
    }

    // Leaving the worker sends the same null-activity frame as leaving
    // fullscreen and then drops the handle, so a clean exit cannot leave a ghost
    // presence in the user's Discord. It also runs after a fault, and the frame
    // build is guarded: it is the last thing this thread does, so an exception
    // escaping here terminates just as hard as one from the loop.
    if (pipe_) {
        try {
            std::ostringstream payload;
            payload << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" << GetCurrentProcessId()
                    << ",\"activity\":null},\"nonce\":\"" << nonceString() << "\"}";
            sendFrame(kOpcodeFrame, payload.str());
        } catch (const std::exception& error) {
            Logger::instance().warning(L"discord",
                L"Discord activity clear failed before exit: " + utf8ToWide(error.what()));
        } catch (...) {
            Logger::instance().warning(L"discord", L"Discord activity clear failed before exit (unknown error)");
        }
        disconnect();
    }
}

bool DiscordRichPresence::connect() {
    for (int index = 0; index <= kMaxDiscordPipeIndex; ++index) {
        wchar_t path[64]{};
        std::swprintf(path, _countof(path), L"\\\\?\\pipe\\discord-ipc-%d", index);
        HANDLE pipe = CreateFileW(
            path,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        const DWORD createError = GetLastError();
        if (pipe == INVALID_HANDLE_VALUE && createError == ERROR_PIPE_BUSY) {
            if (WaitNamedPipeW(path, 2000)) {
                pipe = CreateFileW(
                    path,
                    GENERIC_READ | GENERIC_WRITE,
                    0,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);
            }
        }
        if (pipe != INVALID_HANDLE_VALUE) {
            // Byte stream reads: the framing below reads an 8-byte header and
            // then exactly the declared payload length. Message mode would
            // complete a short header read with ERROR_MORE_DATA and discard
            // the rest of Discord's frame, so every handshake reply looked
            // like a dead connection.
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
            pipe_ = pipe;
            Logger::instance().info(L"discord", std::wstring(L"Discord pipe opened: discord-ipc-") + std::to_wstring(index));
            return true;
        }
        if (index == 0) {
            Logger::instance().warning(L"discord",
                std::wstring(L"Discord pipe open failed for discord-ipc-0, GetLastError=") + std::to_wstring(createError));
        }
    }
    Logger::instance().warning(L"discord", L"Discord pipe not found (discord-ipc-0..9). Is the Discord desktop client running?");
    return false;
}

void DiscordRichPresence::disconnect() {
    if (!pipe_) {
        return;
    }
    // No FlushFileBuffers: on a named pipe it does not return until the client
    // has read everything still buffered, so a Discord client that stopped
    // reading (self-update, hung IPC thread) would wedge this worker and, with
    // it, the UI thread blocked in stop()'s join. A frame is a few hundred bytes
    // and CloseHandle already leaves the kernel buffer readable by the server.
    CloseHandle(reinterpret_cast<HANDLE>(pipe_));
    pipe_ = nullptr;
}

bool DiscordRichPresence::handshake() {
    std::string payload = "{\"v\":1,\"client_id\":\"";
    payload += kDiscordApplicationId;
    payload += "\"}";
    const bool ok = sendFrame(kOpcodeHandshake, payload);
    if (!ok) {
        Logger::instance().warning(L"discord", L"Discord handshake write failed");
    }
    return ok;
}

bool DiscordRichPresence::sendFrame(int opcode, const std::string& payload) {
    if (!pipe_) {
        return false;
    }
    const auto op = static_cast<uint32_t>(opcode);
    const auto len = static_cast<uint32_t>(payload.size());
    std::string frame;
    frame.resize(8 + payload.size());
    std::memcpy(frame.data(), &op, sizeof(op));
    std::memcpy(frame.data() + 4, &len, sizeof(len));
    std::memcpy(frame.data() + 8, payload.data(), payload.size());

    const HANDLE handle = reinterpret_cast<HANDLE>(pipe_);
    const DWORD total = static_cast<DWORD>(frame.size());
    DWORD done = 0;
    while (done < total) {
        DWORD got = 0;
        if (!WriteFile(handle, frame.data() + done, total - done, &got, nullptr) ||
            got == 0) {
            return false;
        }
        done += got;
    }
    return done == total;
}

bool DiscordRichPresence::drainInbox() {
    if (!pipe_) {
        return false;
    }
    uint8_t header[8];

    while (true) {
        DWORD bytesAvailable = 0;
        if (!PeekNamedPipe(reinterpret_cast<HANDLE>(pipe_), nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
            return false;
        }
        if (bytesAvailable == 0) {
            return true;
        }
        if (bytesAvailable < sizeof(header)) {
            // Partial header still in flight; wait for the rest next pass.
            return true;
        }
        if (!readExact(reinterpret_cast<HANDLE>(pipe_), header, sizeof(header))) {
            return false;
        }
        uint32_t opcode = 0;
        uint32_t length = 0;
        std::memcpy(&opcode, header, sizeof(opcode));
        std::memcpy(&length, header + 4, sizeof(length));

        if (length > kMaxInboundFrameBytes) {
            Logger::instance().warning(L"discord",
                L"Discord IPC frame declares oversized length " + std::to_wstring(length) + L"; dropping connection");
            return false;
        }

        std::string payload(length, '\0');
        if (!payload.empty()) {
            if (!readExact(reinterpret_cast<HANDLE>(pipe_), payload.data(), static_cast<DWORD>(payload.size()))) {
                return false;
            }
        }
        std::wstring widePayload = utf8ToWide(payload);
        Logger::instance().debug(L"discord",
            std::wstring(L"Discord IPC recv opcode=") + std::to_wstring(opcode) + L" payload=" + widePayload);

        if (opcode == kOpcodeClose) {
            return false;
        }
    }
}

bool DiscordRichPresence::updateActivity(const std::string& gameName, long long startUnixSeconds) {
    const std::string details = gameName.empty()
        ? std::string("Clipping with Backtrack")
        : std::string("Clipping ") + jsonEscape(gameName);
    std::ostringstream payload;
    payload << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" << GetCurrentProcessId() << ",\"activity\":{";
    payload << "\"details\":\"" << details << "\",";
    payload << "\"timestamps\":{\"start\":" << startUnixSeconds << "},";
    payload << "\"buttons\":[{\"label\":\"" << jsonEscape(kGithubButtonLabel)
            << "\",\"url\":\"" << jsonEscape(kGithubButtonUrl) << "\"}]";
    payload << "}},\"nonce\":\"" << nonceString() << "\"}";
    return sendFrame(kOpcodeFrame, payload.str());
}

} // namespace backtrack
