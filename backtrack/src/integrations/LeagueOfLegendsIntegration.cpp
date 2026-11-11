#include "integrations/LeagueOfLegendsIntegration.h"

#include "core/LogContext.h"
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

struct EventFeed {
    // Only events newer than the last seen id/time; the worker scans these
    // for kills. maxEventId/maxEventTime cover the whole snapshot (falling
    // back to the last seen values when it is empty) so the baseline-reset
    // logic still sees every event, including the already-seen history.
    std::vector<LiveEvent> events;
    int64_t maxEventId = -1;
    double maxEventTime = 0.0;
};

bool isNewEvent(int64_t eventId, double eventTime, int64_t lastEventId, double lastEventTime) {
    if (eventId > lastEventId) {
        return true;
    }
    if (eventId == lastEventId && eventTime > lastEventTime + 0.001) {
        return true;
    }
    if (lastEventId <= 0 && eventTime > lastEventTime + 0.001) {
        return true;
    }
    return false;
}

std::optional<int64_t> jsonIntField(std::string_view object, std::string_view field) {
    const auto valuePos = jsonValuePos(object, field);
    if (!valuePos) {
        return std::nullopt;
    }
    size_t pos = *valuePos;
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
    const auto valuePos = jsonValuePos(object, field);
    if (!valuePos) {
        return std::nullopt;
    }
    size_t pos = *valuePos;
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

EventFeed parseEvents(std::string_view json, int64_t lastEventId, double lastEventTime) {
    EventFeed feed;
    // An empty snapshot has no maxima and must not erase the baseline.
    feed.maxEventId = lastEventId;
    feed.maxEventTime = lastEventTime;
    bool sawEvent = false;

    const size_t eventsKey = json.find("\"Events\"");
    if (eventsKey == std::string_view::npos) {
        return feed;
    }
    const size_t arrayStart = json.find('[', eventsKey);
    if (arrayStart == std::string_view::npos) {
        return feed;
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
                const int64_t eventId = jsonIntField(object, "EventID").value_or(-1);
                const double eventTime = jsonDoubleField(object, "EventTime").value_or(0.0);
                if (!sawEvent) {
                    feed.maxEventId = eventId;
                    feed.maxEventTime = eventTime;
                    sawEvent = true;
                } else {
                    feed.maxEventId = std::max(feed.maxEventId, eventId);
                    feed.maxEventTime = std::max(feed.maxEventTime, eventTime);
                }
                // The feed is ordered by event; only events past the last
                // seen id/time can be new, so extract their names here and
                // skip the (growing) already-seen history.
                if (isNewEvent(eventId, eventTime, lastEventId, lastEventTime)) {
                    LiveEvent event;
                    event.eventId = eventId;
                    event.eventTime = eventTime;
                    event.eventName = jsonStringField(object, "EventName").value_or(L"");
                    event.killerName = jsonStringField(object, "KillerName").value_or(L"");
                    feed.events.push_back(std::move(event));
                }
                objectStart = std::string_view::npos;
            }
        }
    }

    return feed;
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
    const LogContext::ScopedTask taskContext(L"league");
    Logger::instance().info(L"league", L"League of Legends integration started");

    try {
        bool gameAvailable = false;
        bool baselineReady = false;
        int64_t lastEventId = -1;
        double lastEventTime = 0.0;
        // Persistent-outage detection: warn once when two consecutive polls
        // fail (one warning per outage episode; reset on recovery).
        int consecutiveFailures = 0;

        // a cancelled request retains its old session and connection until
        // WinHTTP finishes closing it; later polls use a fresh pair on failure.
        std::shared_ptr<AsyncHttpHandles> handles;

        while (!stopRequested_.load(std::memory_order_acquire)) {
            if (!handles) {
                handles = std::make_shared<AsyncHttpHandles>();
                // loopback must never be redirected through a system proxy.
                handles->session.reset(WinHttpOpen(
                    L"Backtrack/1.0",
                    WINHTTP_ACCESS_TYPE_NO_PROXY,
                    WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS,
                    WINHTTP_FLAG_ASYNC));
                if (handles->session) {
                    WinHttpSetTimeouts(handles->session.value, 1000, 1000, 1000, 1000);
                    handles->connection.reset(WinHttpConnect(handles->session.value, kHost, kPort, 0));
                }
            }
            CancellableHttpSession session{handles, &stopRequested_};
            auto activePlayer = httpGetBody(session, handles->connection.value, L"/liveclientdata/activeplayer", nullptr, true, L"league");
            // activeplayer is the liveness probe; don't request events after it fails.
            auto eventData = activePlayer && !stopRequested_.load(std::memory_order_acquire)
                ? httpGetBody(session, handles->connection.value, L"/liveclientdata/eventdata", nullptr, true, L"league")
                : std::nullopt;
            if (!activePlayer || !eventData) {
                handles.reset();
                if (gameAvailable) {
                    Logger::instance().info(L"league", L"League of Legends live client data is no longer available");
                }
                gameAvailable = false;
                ++consecutiveFailures;
                if (consecutiveFailures == 2) {
                    Logger::instance().warning(L"league", L"League live client data unavailable (persistent HTTP failure)");
                }
                setStats(false, {});
                baselineReady = false;
                lastEventId = -1;
                lastEventTime = 0.0;
                sleepUntilNextPoll(stopRequested_, kUnavailablePollInterval);
                continue;
            }

            const auto names = activePlayerNames(*activePlayer);
            // parseEvents tracks the whole-snapshot maxima and only materializes
            // events newer than the last seen id/time, so a long match's
            // already-seen history is not re-extracted every poll.
            const auto feed = parseEvents(*eventData, lastEventId, lastEventTime);
            const auto& events = feed.events;
            const int64_t maxEventId = feed.maxEventId;
            const double maxEventTime = feed.maxEventTime;

            if (!gameAvailable) {
                Logger::instance().info(L"league", L"League of Legends live client data detected");
            }
            gameAvailable = true;
            consecutiveFailures = 0;
            setStats(true, {});

            if (!baselineReady || maxEventId < lastEventId || maxEventTime + 0.001 < lastEventTime) {
                lastEventId = maxEventId;
                lastEventTime = maxEventTime;
                baselineReady = true;
                sleepUntilNextPoll(stopRequested_, kPollInterval);
                continue;
            }

            // Every materialized event already passed the isNewEvent gate in
            // parseEvents, so only the kill match remains.
            for (const auto& event : events) {
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
