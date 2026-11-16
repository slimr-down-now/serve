#pragma once

#include "serve_daemon.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace serve_daemon_server_test {

struct TempDirGuard {
    std::filesystem::path dir;
    explicit TempDirGuard(std::string_view prefix) {
        dir = std::filesystem::path("build") / (std::string(prefix) + "-" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
    }
    TempDirGuard(const TempDirGuard &) = delete;
    TempDirGuard &operator=(const TempDirGuard &) = delete;
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

} // namespace serve_daemon_server_test

TEST_CASE("[daemon-server] requires at least one opt-in listener and validates sun_path") {
    slimr::ServeApp app("daemon-test", "0.1.0");
    slimr::DaemonServer noListener(app, slimr::DaemonListenOptions{}, nullptr);
    std::string err;
    CHECK_FALSE(noListener.start(&err));
    CHECK(err.find("No listener") != std::string::npos);

    slimr::DaemonListenOptions tooLong;
    tooLong.socketPath = std::string(sizeof(sockaddr_un::sun_path) + 8, 'a');
    slimr::DaemonServer badPath(app, tooLong, nullptr);
    err.clear();
    CHECK_FALSE(badPath.start(&err));
    CHECK(err.find("sun_path") != std::string::npos);
}

TEST_CASE("[daemon-server] single-instance flock guard prevents concurrent bind and unlinks on stop") {
    serve_daemon_server_test::TempDirGuard tmp("fk");
    auto sockPath = (tmp.dir / "s.sock").string();

    slimr::ServeApp app("daemon-flock", "0.1.0");
    slimr::DaemonListenOptions opts;
    opts.socketPath = sockPath;
    opts.socketMode = 0600;

    slimr::DaemonServer first(app, opts, [](httplib::Server &svr, slimr::DisconnectWatcher &, bool) {
        svr.Get("/ping", [](const httplib::Request &, httplib::Response &res) {
            res.set_content("ok", "text/plain");
        });
    });

    std::string err;
    REQUIRE(first.start(&err));
    CHECK(std::filesystem::exists(sockPath));

    struct stat st{};
    REQUIRE(::stat(sockPath.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    slimr::DaemonServer second(app, opts, nullptr);
    std::string secondErr;
    CHECK_FALSE(second.start(&secondErr));
    CHECK(secondErr.find("lock") != std::string::npos);

    first.stop();
    CHECK_FALSE(std::filesystem::exists(sockPath));
}

TEST_CASE("[daemon-server] dual TCP and Unix socket listeners serve identical routes") {
    serve_daemon_server_test::TempDirGuard tmp("dl");
    auto sockPath = (tmp.dir / "d.sock").string();

    slimr::ServeApp app("daemon-dual", "0.1.0");
    slimr::DaemonListenOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    opts.socketPath = sockPath;

    slimr::DaemonServer server(app, opts, [](httplib::Server &svr, slimr::DisconnectWatcher &, bool isUnix) {
        svr.Get("/who", [isUnix](const httplib::Request &, httplib::Response &res) {
            res.set_content(isUnix ? "unix" : "tcp", "text/plain");
        });
    });

    std::string err;
    REQUIRE(server.start(&err));
    REQUIRE(server.boundTcpPort() > 0);

    httplib::Client tcpCli("127.0.0.1", server.boundTcpPort());
    auto tcpRes = tcpCli.Get("/who");
    REQUIRE(tcpRes);
    CHECK(tcpRes->status == 200);
    CHECK(tcpRes->body == "tcp");

    httplib::Client unixCli(sockPath);
    unixCli.set_address_family(AF_UNIX);
    auto unixRes = unixCli.Get("/who");
    REQUIRE(unixRes);
    CHECK(unixRes->status == 200);
    CHECK(unixRes->body == "unix");

    server.stop();
    CHECK_FALSE(std::filesystem::exists(sockPath));
}

TEST_CASE("[daemon-server] onServe receives stop_token before shutdown and exceptions trigger shutdown") {
    slimr::ServeApp app("daemon-onserve", "0.1.0");
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    std::mutex mu;
    std::condition_variable_any cv;

    app.onServe = [&](std::stop_token st) {
        started.store(true, std::memory_order_release);
        std::unique_lock lk(mu);
        cv.wait(lk, st, [&st] { return st.stop_requested(); });
        stopped.store(true, std::memory_order_release);
    };

    slimr::DaemonListenOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    slimr::DaemonServer server(app, opts, nullptr);
    std::string err;
    REQUIRE(server.start(&err));

    for (int i = 0; i < 40 && !started.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(started.load(std::memory_order_acquire));
    server.stop();
    CHECK(stopped.load(std::memory_order_acquire));

    slimr::ServeApp errApp("daemon-onserve-err", "0.1.0");
    errApp.onServe = [](std::stop_token) {
        throw std::runtime_error("boom Bearer secret123");
    };
    slimr::DaemonServer errServer(errApp, opts, nullptr);
    REQUIRE(errServer.start(&err));
    for (int i = 0; i < 40 && !errServer.isShutdownRequested(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(errServer.isShutdownRequested());
    errServer.stop();
}

TEST_CASE("[daemon-server] cancel watcher cancels handler on client disconnect") {
    serve_daemon_server_test::TempDirGuard tmp("cn");
    auto sockPath = (tmp.dir / "c.sock").string();

    slimr::ServeApp app("daemon-cancel", "0.1.0");
    slimr::DaemonListenOptions opts;
    opts.socketPath = sockPath;
    opts.cancelPollInterval = std::chrono::milliseconds{20};

    std::atomic<bool> handlerEntered{false};
    std::atomic<bool> handlerCancelled{false};
    std::mutex mu;
    std::condition_variable cv;

    slimr::DaemonServer server(app, opts, [&](httplib::Server &svr, slimr::DisconnectWatcher &watcher, bool) {
        svr.Get("/wait", [&](const httplib::Request &req, httplib::Response &res) {
            auto cancelSrc = std::make_shared<slimr::CancellationSource>();
            auto guard = watcher.watch(req, cancelSrc);
            auto tok = cancelSrc->token();
            auto cbId = tok.onCancel([&]() {
                std::lock_guard lk(mu);
                handlerCancelled.store(true, std::memory_order_release);
                cv.notify_all();
            });
            handlerEntered.store(true, std::memory_order_release);

            {
                std::unique_lock lk(mu);
                cv.wait_for(lk, std::chrono::seconds(5), [&] {
                    return handlerCancelled.load(std::memory_order_acquire);
                });
            }
            tok.removeOnCancel(cbId);
            res.set_content(handlerCancelled.load() ? "cancelled" : "timeout", "text/plain");
        });
    });

    std::string err;
    REQUIRE(server.start(&err));

    std::thread clientThread([&sockPath]() {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, sockPath.c_str(), sockPath.size() + 1);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            const char reqBytes[] = "GET /wait HTTP/1.1\r\nHost: localhost\r\n\r\n";
            (void)::send(fd, reqBytes, sizeof(reqBytes) - 1, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }
        ::close(fd);
    });

    for (int i = 0; i < 50 && !handlerEntered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(handlerEntered.load(std::memory_order_acquire));

    clientThread.join();
    for (int i = 0; i < 50 && !handlerCancelled.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(handlerCancelled.load(std::memory_order_acquire));

    server.stop();
}
