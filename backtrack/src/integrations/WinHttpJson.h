#pragma once

// shared WinHTTP GET and JSON string reader for the update check and League.
#include <Windows.h>
#include <winhttp.h>

#include "core/Logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace backtrack {

// 4 MiB dwarfs any real payload from either endpoint; reaching the cap means
// something other than the expected server owns the port, so refuse the body
// rather than buffer it.
inline constexpr size_t kMaxHttpResponseBytes = 4 * 1024 * 1024;

struct InternetHandle {
    HINTERNET value = nullptr;

    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle)
        : value(handle) {
    }
    ~InternetHandle() {
        if (value) {
            WinHttpCloseHandle(value);
        }
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    void reset(HINTERNET handle = nullptr) {
        if (value) {
            WinHttpCloseHandle(value);
        }
        value = handle;
    }

    explicit operator bool() const {
        return value != nullptr;
    }
};

struct AsyncHttpHandles {
    InternetHandle session;
    InternetHandle connection;
};

struct CancellableHttpSession {
    std::shared_ptr<AsyncHttpHandles> handles;
    const std::atomic<bool>* stopRequested = nullptr;
    std::chrono::milliseconds timeout{5000};
    bool secure = true;

    operator HINTERNET() const {
        return handles ? handles->session.value : nullptr;
    }
};

inline int jsonHexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

inline size_t skipJsonWhitespace(std::string_view json, size_t pos) {
    while (pos < json.size()) {
        const char ch = json[pos];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        ++pos;
    }
    return pos;
}

// Parses a JSON string at quotePos, decoding standard escapes including \u.
// Raw UTF-8 bytes pass through in chunks so multibyte text survives.
inline std::optional<std::wstring> parseJsonStringAt(std::string_view json, size_t quotePos, size_t* endPos = nullptr) {
    if (quotePos >= json.size() || json[quotePos] != '"') {
        return std::nullopt;
    }

    std::wstring result;
    std::string utf8Chunk;
    auto flushUtf8 = [&] {
        if (!utf8Chunk.empty()) {
            result += utf8ToWide(utf8Chunk);
            utf8Chunk.clear();
        }
    };

    for (size_t index = quotePos + 1; index < json.size(); ++index) {
        const char ch = json[index];
        if (ch == '"') {
            flushUtf8();
            if (endPos) {
                *endPos = index + 1;
            }
            return result;
        }
        if (ch != '\\') {
            utf8Chunk.push_back(ch);
            continue;
        }
        if (index + 1 >= json.size()) {
            return std::nullopt;
        }

        flushUtf8();
        const char escaped = json[++index];
        switch (escaped) {
        case '"':
            result.push_back(L'"');
            break;
        case '\\':
            result.push_back(L'\\');
            break;
        case '/':
            result.push_back(L'/');
            break;
        case 'b':
            result.push_back(L'\b');
            break;
        case 'f':
            result.push_back(L'\f');
            break;
        case 'n':
            result.push_back(L'\n');
            break;
        case 'r':
            result.push_back(L'\r');
            break;
        case 't':
            result.push_back(L'\t');
            break;
        case 'u': {
            if (index + 4 >= json.size()) {
                return std::nullopt;
            }
            uint32_t code = 0;
            for (int digit = 0; digit < 4; ++digit) {
                const int value = jsonHexValue(json[index + 1 + digit]);
                if (value < 0) {
                    return std::nullopt;
                }
                code = (code << 4) | static_cast<uint32_t>(value);
            }
            index += 4;
            result.push_back(static_cast<wchar_t>(code));
            break;
        }
        default:
            result.push_back(static_cast<wchar_t>(escaped));
            break;
        }
    }

    return std::nullopt;
}

inline std::optional<std::wstring> jsonStringField(std::string_view object, std::string_view field) {
    const std::string pattern = "\"" + std::string(field) + "\"";
    size_t pos = object.find(pattern);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos = object.find(':', pos + pattern.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos = skipJsonWhitespace(object, pos + 1);
    return parseJsonStringAt(object, pos);
}

struct AsyncHttpRequestState {
    std::shared_ptr<AsyncHttpHandles> handles;
    std::array<char, 8192> buffer{};
    std::mutex mutex;
    std::condition_variable completed;
    DWORD status = 0;
    DWORD bytesRead = 0;
};

inline void CALLBACK httpRequestCallback(HINTERNET, DWORD_PTR context, DWORD status, void*, DWORD length) {
    auto* owner = reinterpret_cast<std::shared_ptr<AsyncHttpRequestState>*>(context);
    if (!owner) return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        delete owner;
        return;
    }
    if (status != WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE &&
        status != WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE &&
        status != WINHTTP_CALLBACK_STATUS_READ_COMPLETE &&
        status != WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) return;
    auto state = *owner;
    {
        std::scoped_lock lock(state->mutex);
        state->status = status;
        state->bytesRead = length;
    }
    state->completed.notify_one();
}

// request storage and parents outlive cancellation until WinHTTP's final callback.
inline std::optional<std::string> httpGetBody(
    const CancellableHttpSession& session, HINTERNET connection, const wchar_t* path,
    const wchar_t* extraHeaders, bool ignoreCert, const wchar_t* logSubsystem) {
    const auto deadline = std::chrono::steady_clock::now() + session.timeout;
    if (!session.handles || !session.handles->session || !connection ||
        session.handles->connection.value != connection || !path) return std::nullopt;

    auto state = std::make_shared<AsyncHttpRequestState>();
    state->handles = session.handles;
    InternetHandle request(WinHttpOpenRequest(connection, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, session.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        Logger::instance().debug(logSubsystem, L"HTTP GET request open failed");
        return std::nullopt;
    }

    auto owner = std::make_unique<std::shared_ptr<AsyncHttpRequestState>>(state);
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(owner.get());
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
        WinHttpSetStatusCallback(request.value, httpRequestCallback,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        Logger::instance().debug(logSubsystem, L"HTTP GET callback setup failed");
        return std::nullopt;
    }
    owner.release();

    if (ignoreCert && session.secure) {
        DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request.value, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
    }

    auto cancelled = [&] {
        return (session.stopRequested && session.stopRequested->load(std::memory_order_acquire)) ||
            std::chrono::steady_clock::now() >= deadline;
    };
    auto waitFor = [&](DWORD expected) {
        std::unique_lock lock(state->mutex);
        while (!state->status && !cancelled()) {
            const auto wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            state->completed.wait_until(lock, (std::min)(wake, deadline));
        }
        const bool ok = !cancelled() && state->status == expected;
        state->status = 0;
        return ok;
    };

    if (cancelled() || !WinHttpSendRequest(request.value,
            extraHeaders ? extraHeaders : WINHTTP_NO_ADDITIONAL_HEADERS,
            extraHeaders ? static_cast<DWORD>(-1) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, context) ||
        !waitFor(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE) ||
        !WinHttpReceiveResponse(request.value, nullptr) ||
        !waitFor(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)) {
        Logger::instance().debug(logSubsystem, L"HTTP GET send/receive failed or cancelled");
        return std::nullopt;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX) || statusCode != 200) {
        Logger::instance().debug(logSubsystem, L"HTTP GET unexpected status");
        return std::nullopt;
    }

    std::string body;
    for (;;) {
        if (cancelled() || !WinHttpReadData(request.value, state->buffer.data(),
                static_cast<DWORD>(state->buffer.size()), nullptr) ||
            !waitFor(WINHTTP_CALLBACK_STATUS_READ_COMPLETE)) return std::nullopt;
        const DWORD read = state->bytesRead;
        if (!read) return body;
        if (read > kMaxHttpResponseBytes - body.size()) {
            Logger::instance().warning(logSubsystem, L"HTTP GET body exceeds the 4 MiB cap; refusing");
            return std::nullopt;
        }
        body.append(state->buffer.data(), read);
    }
}

// shortcut: synchronous sessions cannot be actively cancelled; migrate the
// update check to an async session before relying on a total deadline.
inline std::optional<std::string> httpGetBody(
    HINTERNET session, HINTERNET connection, const wchar_t* path,
    const wchar_t* extraHeaders, bool ignoreCert, const wchar_t* logSubsystem) {
    if (!session || !connection || !path) {
        return std::nullopt;
    }

    InternetHandle request(WinHttpOpenRequest(
        connection,
        L"GET",
        path,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request) {
        Logger::instance().debug(logSubsystem, L"HTTP GET request open failed");
        return std::nullopt;
    }

    if (ignoreCert) {
        // Local game client only: its endpoint serves a self-signed cert.
        DWORD securityFlags =
            SECURITY_FLAG_IGNORE_UNKNOWN_CA |
            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
            SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request.value, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
    }

    if (!WinHttpSendRequest(
            request.value,
            extraHeaders ? extraHeaders : WINHTTP_NO_ADDITIONAL_HEADERS,
            extraHeaders ? static_cast<DWORD>(-1) : 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
        Logger::instance().debug(logSubsystem, L"HTTP GET send/receive failed");
        return std::nullopt;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request.value,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusCodeSize,
            WINHTTP_NO_HEADER_INDEX) ||
        statusCode != 200) {
        Logger::instance().debug(logSubsystem, L"HTTP GET unexpected status");
        return std::nullopt;
    }

    // QueryDataAvailable reports each chunk in turn, so chunked bodies read
    // out the same way; the loop ends on the zero-length final chunk.
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) {
            return std::nullopt;
        }
        if (available == 0) {
            break;
        }
        if (body.size() + available > kMaxHttpResponseBytes) {
            Logger::instance().warning(logSubsystem, L"HTTP GET body exceeds the 4 MiB cap; refusing");
            return std::nullopt;
        }
        const size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.value, body.data() + offset, available, &read)) {
            return std::nullopt;
        }
        body.resize(offset + read);
    }
    return body;
}

} // namespace backtrack
