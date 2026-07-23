#include "integrations/LeagueOfLegendsIntegration.h"

#include "core/Logger.h"
#include "platform/Win32Util.h"

#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace backtrack {

namespace {

constexpr wchar_t kHost[] = L"127.0.0.1";
constexpr INTERNET_PORT kPort = 2999;
constexpr auto kPollInterval = std::chrono::milliseconds(1000);
constexpr auto kUnavailablePollInterval = std::chrono::milliseconds(2500);

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

    explicit operator bool() const {
        return value != nullptr;
    }
};

struct LiveEvent {
    int64_t eventId = -1;
    double eventTime = 0.0;
    std::wstring eventName;
    std::wstring killerName;
};

int hexValue(char ch) {
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

std::optional<std::wstring> parseJsonStringAt(std::string_view json, size_t quotePos, size_t* endPos = nullptr) {
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
                const int value = hexValue(json[index + 1 + digit]);
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

size_t skipJsonWhitespace(std::string_view json, size_t pos) {
    while (pos < json.size()) {
        const char ch = json[pos];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        ++pos;
    }
    return pos;
}

std::optional<std::wstring> jsonStringField(std::string_view object, std::string_view field) {
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

std::optional<int64_t> jsonIntField(std::string_view object, std::string_view field) {
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
    const size_t start = pos;
    if (pos < object.size() && object[pos] == '-') {
        ++pos;
    }
    while (pos < object.size() && object[pos] >= '0' && object[pos] <= '9') {
        ++pos;
    }
    if (pos == start) {
        return std::nullopt;
    }

    int64_t value = 0;
    const auto number = object.substr(start, pos - start);
    const auto* first = number.data();
    const auto* last = number.data() + number.size();
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> jsonDoubleField(std::string_view object, std::string_view field) {
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
    const size_t start = pos;
    while (pos < object.size()) {
        const char ch = object[pos];
        if ((ch < '0' || ch > '9') && ch != '-' && ch != '+' && ch != '.' && ch != 'e' && ch != 'E') {
            break;
        }
        ++pos;
    }
    if (pos == start) {
        return std::nullopt;
    }

    const std::string number(object.substr(start, pos - start));
    char* end = nullptr;
    const double value = std::strtod(number.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> httpGet(const wchar_t* path) {
    InternetHandle session(WinHttpOpen(
        L"Backtrack/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session) {
        return std::nullopt;
    }

    WinHttpSetTimeouts(session.value, 1000, 1000, 1000, 1000);

    InternetHandle connection(WinHttpConnect(session.value, kHost, kPort, 0));
    if (!connection) {
        return std::nullopt;
    }

    InternetHandle request(WinHttpOpenRequest(
        connection.value,
        L"GET",
        path,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request) {
        return std::nullopt;
    }

    DWORD securityFlags =
        SECURITY_FLAG_IGNORE_UNKNOWN_CA |
        SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
        SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
        SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(request.value, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));

    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
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
        return std::nullopt;
    }

    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) {
            return std::nullopt;
        }
        if (available == 0) {
            break;
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

std::vector<LiveEvent> parseEvents(std::string_view json) {
    std::vector<LiveEvent> events;

    const size_t eventsKey = json.find("\"Events\"");
    if (eventsKey == std::string_view::npos) {
        return events;
    }
    const size_t arrayStart = json.find('[', eventsKey);
    if (arrayStart == std::string_view::npos) {
        return events;
    }

    bool inString = false;
    bool escaped = false;
    int objectDepth = 0;
    size_t objectStart = std::string_view::npos;

    for (size_t index = arrayStart + 1; index < json.size(); ++index) {
        const char ch = json[index];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }

        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == '[' && objectDepth == 0) {
            continue;
        }
        if (ch == ']' && objectDepth == 0) {
            break;
        }
        if (ch == '{') {
            if (objectDepth == 0) {
                objectStart = index;
            }
            ++objectDepth;
            continue;
        }
        if (ch == '}' && objectDepth > 0) {
            --objectDepth;
            if (objectDepth == 0 && objectStart != std::string_view::npos) {
                const auto object = json.substr(objectStart, index - objectStart + 1);
                LiveEvent event;
                event.eventId = jsonIntField(object, "EventID").value_or(-1);
                event.eventTime = jsonDoubleField(object, "EventTime").value_or(0.0);
                event.eventName = jsonStringField(object, "EventName").value_or(L"");
                event.killerName = jsonStringField(object, "KillerName").value_or(L"");
                events.push_back(std::move(event));
                objectStart = std::string_view::npos;
            }
        }
    }

    return events;
}

std::wstring trimName(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && iswspace(value.back())) {
        value.pop_back();
    }
    return value;
}

std::wstring normalizedName(std::wstring value) {
    value = trimName(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

void addCandidateName(std::vector<std::wstring>& names, std::wstring name) {
    name = normalizedName(std::move(name));
    if (name.empty()) {
        return;
    }
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        names.push_back(name);
    }

    const auto tagSeparator = name.find(L'#');
    if (tagSeparator != std::wstring::npos) {
        std::wstring withoutTag = trimName(name.substr(0, tagSeparator));
        if (!withoutTag.empty() && std::find(names.begin(), names.end(), withoutTag) == names.end()) {
            names.push_back(std::move(withoutTag));
        }
    }
}

std::vector<std::wstring> activePlayerNames(std::string_view activePlayerJson) {
    std::vector<std::wstring> names;
    for (std::string_view field : {"riotId", "summonerName", "riotIdGameName"}) {
        if (auto value = jsonStringField(activePlayerJson, field)) {
            addCandidateName(names, std::move(*value));
        }
    }
    return names;
}

bool isActivePlayerKill(const LiveEvent& event, const std::vector<std::wstring>& activeNames) {
    if (event.eventName != L"ChampionKill" || event.killerName.empty() || activeNames.empty()) {
        return false;
    }
    const std::wstring killer = normalizedName(event.killerName);
    return std::find(activeNames.begin(), activeNames.end(), killer) != activeNames.end();
}

bool isNewEvent(const LiveEvent& event, int64_t lastEventId, double lastEventTime) {
    if (event.eventId > lastEventId) {
        return true;
    }
    if (event.eventId == lastEventId && event.eventTime > lastEventTime + 0.001) {
        return true;
    }
    if (lastEventId <= 0 && event.eventTime > lastEventTime + 0.001) {
        return true;
    }
    return false;
}

void sleepUntilNextPoll(std::atomic<bool>& stopRequested, std::chrono::milliseconds delay) {
    const auto deadline = std::chrono::steady_clock::now() + delay;
    while (!stopRequested.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace

LeagueOfLegendsIntegration::LeagueOfLegendsIntegration(KillCallback killCallback)
    : killCallback_(std::move(killCallback)) {
}

LeagueOfLegendsIntegration::~LeagueOfLegendsIntegration() {
    stop();
}

void LeagueOfLegendsIntegration::start() {
    std::scoped_lock lock(mutex_);
    if (worker_.joinable()) {
        return;
    }
    stopRequested_ = false;
    worker_ = std::thread(&LeagueOfLegendsIntegration::workerLoop, this);
}

void LeagueOfLegendsIntegration::stop() {
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
}

bool LeagueOfLegendsIntegration::running() const {
    return running_.load(std::memory_order_acquire);
}

void LeagueOfLegendsIntegration::workerLoop() {
    setThreadDescriptionSafe(L"Backtrack League integration");
    running_ = true;
    Logger::instance().info(L"league", L"League of Legends integration started");

    bool gameAvailable = false;
    bool baselineReady = false;
    int64_t lastEventId = -1;
    double lastEventTime = 0.0;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        auto activePlayer = httpGet(L"/liveclientdata/activeplayer");
        auto eventData = httpGet(L"/liveclientdata/eventdata");
        if (!activePlayer || !eventData) {
            if (gameAvailable) {
                Logger::instance().info(L"league", L"League of Legends live client data is no longer available");
            }
            gameAvailable = false;
            baselineReady = false;
            lastEventId = -1;
            lastEventTime = 0.0;
            sleepUntilNextPoll(stopRequested_, kUnavailablePollInterval);
            continue;
        }

        const auto names = activePlayerNames(*activePlayer);
        const auto events = parseEvents(*eventData);
        int64_t maxEventId = lastEventId;
        double maxEventTime = lastEventTime;
        for (const auto& event : events) {
            maxEventId = std::max(maxEventId, event.eventId);
            maxEventTime = std::max(maxEventTime, event.eventTime);
        }

        if (!gameAvailable) {
            Logger::instance().info(L"league", L"League of Legends live client data detected");
        }
        gameAvailable = true;

        if (!baselineReady || maxEventId < lastEventId || maxEventTime + 0.001 < lastEventTime) {
            lastEventId = maxEventId;
            lastEventTime = maxEventTime;
            baselineReady = true;
            sleepUntilNextPoll(stopRequested_, kPollInterval);
            continue;
        }

        for (const auto& event : events) {
            if (!isNewEvent(event, lastEventId, lastEventTime)) {
                continue;
            }
            if (isActivePlayerKill(event, names)) {
                Logger::instance().info(L"league", L"League of Legends kill detected for clip reminder");
                if (killCallback_) {
                    killCallback_();
                }
            }
        }

        lastEventId = maxEventId;
        lastEventTime = maxEventTime;
        sleepUntilNextPoll(stopRequested_, kPollInterval);
    }

    Logger::instance().info(L"league", L"League of Legends integration stopped");
    running_ = false;
}

} // namespace backtrack
