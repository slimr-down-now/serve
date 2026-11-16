#pragma once

#ifdef SLIMR_SERVE_HAS_HTTP

#include "serve_relay.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace cli_relay_test {

struct EnvVarGuard {
    std::string name;
    std::optional<std::string> prev;

    EnvVarGuard(std::string n, const char *val) : name(std::move(n)) {
        if (const char *old = std::getenv(name.c_str())) {
            prev = std::string(old);
        }
        if (val != nullptr) {
            ::setenv(name.c_str(), val, 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }

    EnvVarGuard(const EnvVarGuard &) = delete;
    EnvVarGuard &operator=(const EnvVarGuard &) = delete;

    ~EnvVarGuard() {
        if (prev.has_value()) {
            ::setenv(name.c_str(), prev->c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
};

struct StubHttpServer {
    httplib::Server svr;
    std::thread th;
    int port{0};
    std::filesystem::path sockDir;
    std::string sockPath;

    StubHttpServer() = default;
    StubHttpServer(const StubHttpServer &) = delete;
    StubHttpServer &operator=(const StubHttpServer &) = delete;

    void startTcp() {
        port = svr.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        th = std::thread([this]() { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }

    void startUnix(std::string_view prefix) {
        sockDir = std::filesystem::path("build") / (std::string(prefix) + "-" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(sockDir, ec);
        std::filesystem::create_directories(sockDir, ec);
        sockPath = (sockDir / "relay.sock").string();
        svr.set_address_family(AF_UNIX);
        REQUIRE(svr.bind_to_port(sockPath, 80));
        th = std::thread([this]() { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }

    ~StubHttpServer() {
        svr.stop();
        if (th.joinable()) {
            th.join();
        }
        if (!sockDir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(sockDir, ec);
        }
    }
};

struct RelayInput {
    slimr::Field<std::string, "Name"> name;
};

inline slimr::ServeApp makeRelayApp() {
    slimr::ServeApp app("my-daemon", "1.0.0");
    app.apiVersion = 2;
    app.tool<RelayInput>("greet", "Greet", [](const RelayInput &in) {
        return slimr::ServeResult::text("local:" + in.name.value);
    });
    slimr::enable_cli_relay(app);
    return app;
}

} // namespace cli_relay_test

TEST_CASE("[cli-relay] forwards tool call over Unix socket and returns JSON result") {
    cli_relay_test::StubHttpServer stub;
    std::string seenAccept;
    std::string seenApiVer;
    std::string seenBody;
    stub.svr.Post("/greet", [&](const httplib::Request &req, httplib::Response &res) {
        seenAccept = req.get_header_value("Accept");
        seenApiVer = req.get_header_value("X-Slimr-Api-Version");
        seenBody = req.body;
        res.set_content("{\"greeting\":\"hello ada\"}", "application/json");
    });
    stub.startUnix("cr-unix");

    auto app = cli_relay_test::makeRelayApp();
    const auto *def = app.findTool("greet");
    REQUIRE(def != nullptr);

    std::string remoteArg = "unix:" + stub.sockPath;
    char prog[] = "my-daemon";
    char rFlag[] = "--remote";
    char toolArg[] = "greet";
    char *argv[] = {prog, rFlag, remoteArg.data(), toolArg};

    slimr::Json args = slimr::json_obj();
    args["name"] = "ada";
    slimr::RequestContext ctx;

    std::ostringstream out;
    std::ostringstream err;
    int rc = slimr::run_cli_relay(app, *def, args, ctx, 4, argv, out, err);
    CHECK(rc == 0);
    CHECK(err.str().empty());
    CHECK(out.str() == "{\"greeting\":\"hello ada\"}\n");
    CHECK(seenAccept.find("application/x-ndjson") != std::string::npos);
    CHECK(seenApiVer == "2");
    auto parsedReq = slimr::parse_json<slimr::Json>(seenBody);
    REQUIRE(parsedReq.has_value());
    CHECK((*parsedReq)["name"].get<std::string>() == "ada");
}

TEST_CASE("[cli-relay] forwards context.session and streams NDJSON progress and logs to stderr") {
    cli_relay_test::StubHttpServer stub;
    std::string seenBody;
    stub.svr.Post("/greet", [&](const httplib::Request &req, httplib::Response &res) {
        seenBody = req.body;
        res.status = 200;
        res.set_chunked_content_provider(
            "application/x-ndjson",
            [](size_t offset, httplib::DataSink &sink) {
                if (offset == 0) {
                    const std::string chunk1 =
                        "{\"type\":\"progress\",\"progress\":1,\"total\":2,\"message\":\"working\"}\n"
                        "{\"type\":\"log\",\"level\":\"info\",\"data\":{\"step\":1}}\n";
                    sink.write(chunk1.data(), chunk1.size());
                    const std::string chunk2 =
                        "{\"type\":\"result\",\"data\":{\"ok\":true,\"from\":\"daemon\"}}\n";
                    sink.write(chunk2.data(), chunk2.size());
                    sink.done();
                }
                return true;
            });
    });
    stub.startTcp();

    auto app = cli_relay_test::makeRelayApp();
    app.remote = "http://127.0.0.1:" + std::to_string(stub.port);
    const auto *def = app.findTool("greet");
    REQUIRE(def != nullptr);

    // Env var overrides app.remote; --remote overrides env var.
    cli_relay_test::EnvVarGuard envGuard("MY_DAEMON_REMOTE", app.remote.c_str());

    char prog[] = "my-daemon";
    char toolArg[] = "greet";
    char *argv[] = {prog, toolArg};

    slimr::Json args = slimr::json_obj();
    args["name"] = "bob";
    slimr::RequestContext ctx;
    ctx.session = "sess-42";

    std::ostringstream out;
    std::ostringstream err;
    int rc = slimr::run_cli_relay(app, *def, args, ctx, 2, argv, out, err);
    CHECK(rc == 0);

    auto parsedReq = slimr::parse_json<slimr::Json>(seenBody);
    REQUIRE(parsedReq.has_value());
    CHECK((*parsedReq)["input"]["name"].get<std::string>() == "bob");
    CHECK((*parsedReq)["context"]["session"].get<std::string>() == "sess-42");

    CHECK(err.str().find("\"type\":\"progress\"") != std::string::npos);
    CHECK(err.str().find("\"type\":\"log\"") != std::string::npos);
    auto parsedOut = slimr::parse_json<slimr::Json>(out.str());
    REQUIRE(parsedOut.has_value());
    CHECK((*parsedOut)["ok"].get<bool>() == true);
    CHECK((*parsedOut)["from"].get<std::string>() == "daemon");
}

TEST_CASE("[cli-relay] target precedence chooses --remote over env var over app.remote") {
    cli_relay_test::StubHttpServer flagStub;
    flagStub.svr.Post("/greet", [](const httplib::Request &, httplib::Response &res) {
        res.set_content("{\"via\":\"flag\"}", "application/json");
    });
    flagStub.startTcp();

    cli_relay_test::StubHttpServer envStub;
    envStub.svr.Post("/greet", [](const httplib::Request &, httplib::Response &res) {
        res.set_content("{\"via\":\"env\"}", "application/json");
    });
    envStub.startTcp();

    auto app = cli_relay_test::makeRelayApp();
    app.remote = "http://127.0.0.1:1"; // unreachable fallback
    const auto *def = app.findTool("greet");
    REQUIRE(def != nullptr);

    std::string envUrl = "http://127.0.0.1:" + std::to_string(envStub.port);
    cli_relay_test::EnvVarGuard envGuard("MY_DAEMON_REMOTE", envUrl.c_str());

    char prog[] = "my-daemon";
    char toolArg[] = "greet";
    char *envArgv[] = {prog, toolArg};

    slimr::Json args = slimr::json_obj();
    args["name"] = "x";
    slimr::RequestContext ctx;

    std::ostringstream out1;
    std::ostringstream err1;
    CHECK(slimr::run_cli_relay(app, *def, args, ctx, 2, envArgv, out1, err1) == 0);
    CHECK(out1.str() == "{\"via\":\"env\"}\n");

    std::string flagVal = "--remote=http://127.0.0.1:" + std::to_string(flagStub.port);
    char *flagArgv[] = {prog, flagVal.data(), toolArg};
    std::ostringstream out2;
    std::ostringstream err2;
    CHECK(slimr::run_cli_relay(app, *def, args, ctx, 3, flagArgv, out2, err2) == 0);
    CHECK(out2.str() == "{\"via\":\"flag\"}\n");
}

TEST_CASE("[cli-relay] returns non-zero with clear message on HTTP 426 apiVersion mismatch") {
    cli_relay_test::StubHttpServer stub;
    stub.svr.Post("/greet", [](const httplib::Request &, httplib::Response &res) {
        res.status = 426;
        res.set_content("{\"error\":\"upgrade_required\",\"message\":\"daemon older than client; restart it\"}",
                        "application/json");
    });
    stub.startTcp();

    auto app = cli_relay_test::makeRelayApp();
    app.apiVersion = 5;
    app.remote = "http://127.0.0.1:" + std::to_string(stub.port);
    const auto *def = app.findTool("greet");
    REQUIRE(def != nullptr);

    char prog[] = "my-daemon";
    char toolArg[] = "greet";
    char *argv[] = {prog, toolArg};
    slimr::RequestContext ctx;
    std::ostringstream out;
    std::ostringstream err;

    int rc = slimr::run_cli_relay(app, *def, slimr::json_obj(), ctx, 2, argv, out, err);
    CHECK(rc == 1);
    CHECK(out.str().empty());
    CHECK(err.str().find("426") != std::string::npos);
    CHECK(err.str().find("daemon older than client") != std::string::npos);
}

TEST_CASE("[cli-relay] exits 69 with brew services start hint when daemon is unreachable") {
    auto app = cli_relay_test::makeRelayApp();
    app.remote = "unix:build/nonexistent-cli-relay-test.sock";
    const auto *def = app.findTool("greet");
    REQUIRE(def != nullptr);

    char prog[] = "my-daemon";
    char toolArg[] = "greet";
    char *argv[] = {prog, toolArg};
    slimr::RequestContext ctx;
    std::ostringstream out;
    std::ostringstream err;

    int rc = slimr::run_cli_relay(app, *def, slimr::json_obj(), ctx, 2, argv, out, err);
    CHECK(rc == 69);
    CHECK(out.str().empty());
    CHECK(err.str().find("brew services start my-daemon") != std::string::npos);
}

TEST_CASE("[cli-relay] sends Authorization Bearer header from --bearer-env and <APP>_BEARER") {
    cli_relay_test::StubHttpServer stub;
    std::string seenAuth;
    stub.svr.Post("/greet", [&](const httplib::Request &req, httplib::Response &res) {
        seenAuth = req.get_header_value("Authorization");
        res.set_content("{\"authed\":true}", "application/json");
    });
    stub.startTcp();

    auto app = cli_relay_test::makeRelayApp();
    app.remote = "http://127.0.0.1:" + std::to_string(stub.port);
    const auto *def = app.findTool("greet");
    REQUIRE(def != nullptr);

    // 1. Default <APP>_BEARER env var
    {
        cli_relay_test::EnvVarGuard tokGuard("MY_DAEMON_BEARER", "default-secret-tok");
        char prog[] = "my-daemon";
        char toolArg[] = "greet";
        char *argv[] = {prog, toolArg};
        slimr::RequestContext ctx;
        std::ostringstream out;
        std::ostringstream err;
        int rc = slimr::run_cli_relay(app, *def, slimr::json_obj(), ctx, 2, argv, out, err);
        CHECK(rc == 0);
        CHECK(seenAuth == "Bearer default-secret-tok");
    }

    // 2. Explicit --bearer-env overrides <APP>_BEARER
    {
        cli_relay_test::EnvVarGuard customGuard("CUSTOM_RELAY_TOK", "custom-secret-tok");
        char prog[] = "my-daemon";
        char bFlag[] = "--bearer-env";
        char bVar[] = "CUSTOM_RELAY_TOK";
        char toolArg[] = "greet";
        char *argv[] = {prog, bFlag, bVar, toolArg};
        slimr::RequestContext ctx;
        std::ostringstream out;
        std::ostringstream err;
        int rc = slimr::run_cli_relay(app, *def, slimr::json_obj(), ctx, 4, argv, out, err);
        CHECK(rc == 0);
        CHECK(seenAuth == "Bearer custom-secret-tok");
    }

    // 3. Explicit --bearer-env with unset env var fails cleanly
    {
        cli_relay_test::EnvVarGuard missingGuard("MISSING_RELAY_TOK", nullptr);
        char prog[] = "my-daemon";
        char bFlag[] = "--bearer-env";
        char bVar[] = "MISSING_RELAY_TOK";
        char toolArg[] = "greet";
        char *argv[] = {prog, bFlag, bVar, toolArg};
        slimr::RequestContext ctx;
        std::ostringstream out;
        std::ostringstream err;
        int rc = slimr::run_cli_relay(app, *def, slimr::json_obj(), ctx, 4, argv, out, err);
        CHECK(rc == 1);
        CHECK(err.str().find("MISSING_RELAY_TOK") != std::string::npos);
    }
}

TEST_CASE("[cli-relay] Ctrl-C SIGINT cancels in-flight relay request and returns 130") {
    std::mutex mu;
    std::condition_variable cv;
    bool handlerEntered = false;
    std::atomic<bool> stopServerWait{false};
    cli_relay_test::StubHttpServer stub;

    stub.svr.Post("/greet", [&](const httplib::Request &req, httplib::Response &res) {
        {
            std::lock_guard lock(mu);
            handlerEntered = true;
        }
        cv.notify_all();
        for (int i = 0; i < 100 && !stopServerWait.load(); ++i) {
            if (req.is_connection_closed()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        res.set_content("{\"done\":true}", "application/json");
    });
    stub.startTcp();

    auto app = cli_relay_test::makeRelayApp();
    app.remote = "http://127.0.0.1:" + std::to_string(stub.port);
    const auto *def = app.findTool("greet");
    REQUIRE(def != nullptr);

    std::thread sigThread([&]() {
        {
            std::unique_lock lock(mu);
            cv.wait_for(lock, std::chrono::seconds(2), [&]() { return handlerEntered; });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        std::raise(SIGINT);
    });

    char prog[] = "my-daemon";
    char toolArg[] = "greet";
    char *argv[] = {prog, toolArg};
    slimr::RequestContext ctx;
    std::ostringstream out;
    std::ostringstream err;

    int rc = slimr::run_cli_relay(app, *def, slimr::json_obj(), ctx, 2, argv, out, err);
    stopServerWait.store(true);
    sigThread.join();

    CHECK(rc == 130);
    CHECK(err.str().find("Cancelled") != std::string::npos);
}

#endif
