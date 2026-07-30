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
constexpr auto kActivityUpdateInterval = std::chrono::seconds(15);

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

} // namespace

DiscordRichPresence::~DiscordRichPresence() {
    stop();
}

void DiscordRichPresence::start(GameIntegrationSettings::DiscordRichPresenceMode mode) {
    std::scoped_lock lock(mutex_);
    mode_.store(mode, std::memory_order_release);
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
    running_ = false;
    disconnect();
}

bool DiscordRichPresence::running() const {
    return running_.load(std::memory_order_acquire);
}

void DiscordRichPresence::workerLoop() {
    setThreadDescriptionSafe(L"Backtrack Discord integration");
    running_ = true;
    Logger::instance().info(L"discord", L"Discord Rich Presence integration started");

    std::string lastGameName;
    bool activityShown = false;
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
                    if (!updateActivity(gameName)) {
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

    if (pipe_) {
        std::ostringstream payload;
        payload << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" << GetCurrentProcessId()
                << ",\"activity\":null},\"nonce\":\"" << nonceString() << "\"}";
        sendFrame(kOpcodeFrame, payload.str());
        disconnect();
    }
    running_ = false;
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
            DWORD mode = PIPE_READMODE_MESSAGE;
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
    if (pipe_) {
        const HANDLE handle = reinterpret_cast<HANDLE>(pipe_);
        FlushFileBuffers(handle);
        CloseHandle(handle);
        pipe_ = nullptr;
    }
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

    DWORD written = 0;
    const HANDLE handle = reinterpret_cast<HANDLE>(pipe_);
    if (!WriteFile(handle, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr)) {
        return false;
    }
    return written == frame.size();
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
        DWORD read = 0;
        if (!ReadFile(reinterpret_cast<HANDLE>(pipe_), header, sizeof(header), &read, nullptr)) {
            return false;
        }
        if (read < static_cast<DWORD>(sizeof(header))) {
            return false;
        }
        uint32_t opcode = 0;
        uint32_t length = 0;
        std::memcpy(&opcode, header, sizeof(opcode));
        std::memcpy(&length, header + 4, sizeof(length));

        std::string payload(length, '\0');
        if (!payload.empty()) {
            DWORD payloadRead = 0;
            if (!ReadFile(reinterpret_cast<HANDLE>(pipe_), payload.data(), static_cast<DWORD>(payload.size()), &payloadRead, nullptr)) {
                return false;
            }
            if (payloadRead != payload.size()) {
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

bool DiscordRichPresence::updateActivity(const std::string& gameName) {
    const std::string details = gameName.empty()
        ? std::string("Clipping with Backtrack")
        : std::string("Clipping ") + jsonEscape(gameName);
    std::ostringstream payload;
    payload << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" << GetCurrentProcessId() << ",\"activity\":{";
    payload << "\"details\":\"" << details << "\",";
    payload << "\"timestamps\":{\"start\":" << std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() << "},";
    payload << "\"buttons\":[{\"label\":\"" << jsonEscape(kGithubButtonLabel)
            << "\",\"url\":\"" << jsonEscape(kGithubButtonUrl) << "\"}]";
    payload << "}},\"nonce\":\"" << nonceString() << "\"}";
    return sendFrame(kOpcodeFrame, payload.str());
}

} // namespace backtrack
