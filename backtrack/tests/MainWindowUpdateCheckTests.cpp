#include <winsock2.h>
#include <ws2tcpip.h>

#include "core/AppVersion.h"
#include "core/Logger.h"
#include "ui/MainWindow.h"
#include "ui/UiIds.h"

#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace backtrack {

struct MainWindowUpdateCheckTestAccess {
    static void start(MainWindow& window, HWND target, INTERNET_PORT port, std::chrono::milliseconds timeout) {
        window.window_ = target;
        window.startUpdateCheck({L"127.0.0.1", port, WINHTTP_ACCESS_TYPE_NO_PROXY, false, timeout});
    }
    static bool wait(MainWindow& window, DWORD timeoutMs) {
        return WaitForSingleObject(window.updateCheckThread_.native_handle(), timeoutMs) == WAIT_OBJECT_0;
    }
    static void stop(MainWindow& window) { window.stopUpdateCheck(); }
};

} // namespace backtrack

namespace {

using namespace std::chrono_literals;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Winsock {
    Winsock() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed");
    }
    ~Winsock() { WSACleanup(); }
};

class HttpServer {
public:
    HttpServer(std::string body, std::chrono::milliseconds interval, bool stall)
        : body_(std::move(body)), interval_(interval), stall_(stall) {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) throw std::runtime_error("socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            listen(listener_, 1) == SOCKET_ERROR) {
            closesocket(listener_);
            throw std::runtime_error("listen failed");
        }
        int length = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
            closesocket(listener_);
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    ~HttpServer() {
        stop();
        closesocket(listener_);
    }
    INTERNET_PORT port() const { return port_; }
    std::string request() {
        std::scoped_lock lock(mutex_);
        return request_;
    }
    bool waitForBytes(int count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return sent_.load() >= count; });
    }
    void stop() {
        stopping_.store(true);
        if (worker_.joinable()) worker_.join();
    }

private:
    void serve() {
        SOCKET client = INVALID_SOCKET;
        while (!stopping_.load()) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener_, &set);
            timeval timeout{0, 50000};
            if (select(0, &set, nullptr, nullptr, &timeout) > 0) {
                client = accept(listener_, nullptr, nullptr);
                break;
            }
        }
        if (client == INVALID_SOCKET) return;
        DWORD timeout = 1000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string request;
        char buffer[2048];
        while (!stopping_.load() && request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            const int read = recv(client, buffer, sizeof(buffer), 0);
            if (read <= 0) break;
            request.append(buffer, static_cast<size_t>(read));
        }
        {
            std::scoped_lock lock(mutex_);
            request_ = std::move(request);
        }
        if (request_.find("\r\n\r\n") != std::string::npos) {
            const std::string header = "HTTP/1.1 200 OK\r\nContent-Length: " +
                std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n";
            size_t offset = 0;
            while (offset < header.size() && !stopping_.load()) {
                const int written = send(client, header.data() + offset, static_cast<int>(header.size() - offset), 0);
                if (written <= 0) break;
                offset += static_cast<size_t>(written);
            }
            if (offset == header.size()) {
                offset = 0;
                while (offset < body_.size() && !stopping_.load()) {
                    const size_t chunk = interval_ == 0ms && !stall_ ? body_.size() - offset : 1;
                    const int written = send(client, body_.data() + offset, static_cast<int>(chunk), 0);
                    if (written <= 0) break;
                    offset += static_cast<size_t>(written);
                    {
                        std::scoped_lock lock(mutex_);
                        sent_ += written;
                        changed_.notify_all();
                    }
                    if (stall_) {
                        while (!stopping_.load()) std::this_thread::sleep_for(10ms);
                        break;
                    }
                    const auto next = std::chrono::steady_clock::now() + interval_;
                    while (!stopping_.load() && std::chrono::steady_clock::now() < next) {
                        std::this_thread::sleep_for(10ms);
                    }
                }
            }
        }
        closesocket(client);
    }

    SOCKET listener_ = INVALID_SOCKET;
    INTERNET_PORT port_ = 0;
    std::string body_;
    std::chrono::milliseconds interval_;
    bool stall_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> sent_{0};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string request_;
    std::thread worker_;
};

struct TargetWindow {
    HWND handle = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    TargetWindow() { expect(handle != nullptr, "could not create message target"); }
    ~TargetWindow() { DestroyWindow(handle); }
    bool takeUpdate(WPARAM* result = nullptr) const {
        MSG message{};
        if (!PeekMessageW(&message, handle, backtrack::ui_ids::kUpdateCheckCompleteMessage,
                          backtrack::ui_ids::kUpdateCheckCompleteMessage, PM_REMOVE)) return false;
        if (result) *result = message.wParam;
        return true;
    }
};

struct TestWindow {
    backtrack::RecorderController controller;
    backtrack::SettingsStore store{std::filesystem::temp_directory_path() / L"backtrack_update_test.ini"};
    backtrack::AppSettings settings;
    backtrack::MainWindow window{controller, store, settings, backtrack::SettingsStore::LoadStatus::Missing};
};

void testSuccessfulCheck(bool current) {
    TargetWindow target;
    const auto version = backtrack::wideToUtf8(std::wstring(backtrack::kCurrentAppVersion));
    const auto tag = current ? version : std::string("test-update-version");
    HttpServer server("{\"tag_name\":\"" + tag + "\"}", 0ms, false);
    TestWindow test;
    backtrack::MainWindowUpdateCheckTestAccess::start(test.window, target.handle, server.port(), 3s);
    const bool finished = backtrack::MainWindowUpdateCheckTestAccess::wait(test.window, 5000);
    if (!finished) server.stop();
    backtrack::MainWindowUpdateCheckTestAccess::stop(test.window);
    WPARAM result = 0;
    expect(finished && target.takeUpdate(&result) && result == (current ? 0u : 1u),
           "successful update check did not post the correct version comparison");
    const auto request = server.request();
    expect(request.find("GET /repos/gyatstian/backtrack/releases/latest ") != std::string::npos,
           "update check requested the wrong path");
    expect(request.find("application/vnd.github+json") != std::string::npos,
           "update check omitted the GitHub accept header");
    expect(request.find("Backtrack/" + version) != std::string::npos,
           "update check did not identify the current build");
}

void testSlowProgressHasTotalDeadline() {
    TargetWindow target;
    HttpServer server(std::string(100, 'x'), 80ms, false);
    TestWindow test;
    const auto start = std::chrono::steady_clock::now();
    backtrack::MainWindowUpdateCheckTestAccess::start(test.window, target.handle, server.port(), 700ms);
    const bool progress = server.waitForBytes(3, 2s);
    const bool finished = backtrack::MainWindowUpdateCheckTestAccess::wait(test.window, 2500);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (!finished) server.stop();
    backtrack::MainWindowUpdateCheckTestAccess::stop(test.window);
    expect(progress, "slow server did not make progress");
    expect(finished && elapsed >= 400ms && elapsed < 2500ms, "update deadline did not bound slow progress");
    expect(!target.takeUpdate(), "timed-out check posted an update");
}

void testStopAndDestruction(bool destroy) {
    TargetWindow target;
    HttpServer server(std::string(100, 'x'), 0ms, true);
    auto test = std::make_unique<TestWindow>();
    backtrack::MainWindowUpdateCheckTestAccess::start(test->window, target.handle, server.port(), 5s);
    const bool received = server.waitForBytes(1, 3s);

    std::mutex mutex;
    std::condition_variable changed;
    bool returned = false;
    bool timedOut = false;
    std::thread watchdog([&] {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, 1500ms, [&] { return returned; })) {
            timedOut = true;
            lock.unlock();
            server.stop();
        }
    });
    if (destroy) test.reset();
    else backtrack::MainWindowUpdateCheckTestAccess::stop(test->window);
    {
        std::scoped_lock lock(mutex);
        returned = true;
        changed.notify_one();
    }
    watchdog.join();
    expect(received, "server did not start the pending response");
    expect(!timedOut, destroy ? "window destruction waited for the stalled server" : "stop waited for the stalled server");
    expect(!target.takeUpdate(), "stopped update check posted a result");
}

} // namespace

int main() {
    try {
        Winsock winsock;
        testSuccessfulCheck(false);
        testSuccessfulCheck(true);
        testSlowProgressHasTotalDeadline();
        testStopAndDestruction(false);
        testStopAndDestruction(true);
        std::cout << "MainWindowUpdateCheckTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MainWindowUpdateCheckTests failed: " << error.what() << '\n';
        return 1;
    }
}
