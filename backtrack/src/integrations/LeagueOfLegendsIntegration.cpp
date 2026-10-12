#include "integrations/LeagueOfLegendsIntegration.h"

#include "core/Logger.h"
#include "integrations/WinHttpJson.h"
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

struct LiveEvent {
    int64_t eventId = -1;
    double eventTime = 0.0;
    std::wstring eventName;
    std::wstring killerName;
};

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

std::wstring normalizedName(std::wstring value) {
    value = trimText(std::move(value));
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
        std::wstring withoutTag = trimText(name.substr(0, tagSeparator));
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
}

LeagueIntegrationStats LeagueOfLegendsIntegration::stats() const {
    LeagueIntegrationStats result;
    std::scoped_lock lock(statsMutex_);
    result.gameAvailable = gameAvailable_;
    result.lastError = lastError_;
    return result;
}

void LeagueOfLegendsIntegration::setStats(bool gameAvailable, std::wstring lastError) {
    std::scoped_lock lock(statsMutex_);
    gameAvailable_ = gameAvailable;
    lastError_ = std::move(lastError);
}

void LeagueOfLegendsIntegration::workerLoop() {
    setThreadDescriptionSafe(L"Backtrack League integration");
    Logger::instance().info(L"league", L"League of Legends integration started");

    try {
        bool gameAvailable = false;
        bool baselineReady = false;
        int64_t lastEventId = -1;
        double lastEventTime = 0.0;

        // Pooled across polls: creating a WinHTTP session + TLS connection per
        // request (twice per second) kept the heap hot for no benefit. Both are
        // recreated lazily when missing, and the connection is additionally
        // dropped after any failed request so stale transport state from a
        // restarted game client never lingers.
        InternetHandle session;
        InternetHandle connection;

        while (!stopRequested_.load(std::memory_order_acquire)) {
            if (!session) {
                // Loopback must never leave the machine; a system proxy or VPN would
                // otherwise turn every poll into a remote connection failure.
                session.reset(WinHttpOpen(
                    L"Backtrack/1.0",
                    WINHTTP_ACCESS_TYPE_NO_PROXY,
                    WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS,
                    0));
                if (session) {
                    WinHttpSetTimeouts(session.value, 1000, 1000, 1000, 1000);
                }
            }
            if (session && !connection) {
                connection.reset(WinHttpConnect(session.value, kHost, kPort, 0));
            }
            // Local endpoint with a self-signed cert; polled twice per second, so
            // the session and connection stay pooled and only the connection is
            // dropped after a failure, giving a restarted client fresh state.
            auto activePlayer = httpGetBody(session.value, connection.value, L"/liveclientdata/activeplayer", nullptr, true, L"league");
            // activeplayer is the liveness probe, so when it fails the client is
            // gone: skip the request that would be a guaranteed connection failure.
            auto eventData = activePlayer
                ? httpGetBody(session.value, connection.value, L"/liveclientdata/eventdata", nullptr, true, L"league")
                : std::nullopt;
            if (!activePlayer || !eventData) {
                connection.reset();
                if (gameAvailable) {
                    Logger::instance().info(L"league", L"League of Legends live client data is no longer available");
                }
                gameAvailable = false;
                setStats(false, {});
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
            setStats(true, {});

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
    } catch (const std::exception& error) {
        setStats(false, utf8ToWide(error.what()));
        Logger::instance().warning(L"league",
            std::wstring(L"League of Legends kill reminder disabled by an unexpected error: ") + utf8ToWide(error.what()));
    } catch (...) {
        setStats(false, L"unknown error");
        Logger::instance().warning(L"league", L"League of Legends kill reminder disabled by an unknown error");
    }

    Logger::instance().info(L"league", L"League of Legends integration stopped");
}

} // namespace backtrack
