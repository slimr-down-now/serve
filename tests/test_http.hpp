#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace serve_http_test {

struct AddIn {
    slimr::Field<int, "First"> a;
    slimr::Field<int, "Second"> b;
};

struct AddOut {
    slimr::Field<int, "Sum"> sum;
};

struct SessionIn {
    slimr::OptField<std::string, "Echo"> msg;
};

struct SessionOut {
    slimr::Field<std::string, "Session"> session;
    slimr::Field<std::string, "Msg"> msg;
};

struct EmptyIn {};

inline bool can_connect_loopback(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int rc = ::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    ::close(fd);
    return rc == 0;
}

inline int run_serve(slimr::ServeApp &app, std::vector<std::string> args) {
    std::vector<char *> argv;
    for (auto &a : args)
        argv.push_back(a.data());
    return app.httpRunner()(app, static_cast<int>(argv.size()), argv.data());
}

// Runs the serve runner on a thread; stop() raises SIGTERM once the runner's handler is installed.
struct ServeRunner {
    std::atomic<int> rc{-1};
    std::thread th;

    ServeRunner(slimr::ServeApp &app, std::vector<std::string> args)
        : th([this, &app, args = std::move(args)]() mutable { rc.store(run_serve(app, std::move(args))); }) {}
    ServeRunner(const ServeRunner &) = delete;
    ServeRunner &operator=(const ServeRunner &) = delete;
    ~ServeRunner() { stop(); }

    int stop() {
        for (int i = 0; i < 300 && th.joinable() && rc.load() == -1; ++i) {
            struct sigaction sa{};
            ::sigaction(SIGTERM, nullptr, &sa);
            if (sa.sa_handler != SIG_DFL) {
                std::raise(SIGTERM);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (th.joinable())
            th.join();
        return rc.load();
    }
};

inline httplib::Client unix_client(const std::string &sockPath) {
    httplib::Client cli(sockPath);
    cli.set_address_family(AF_UNIX);
    return cli;
}

inline bool wait_for_health(httplib::Client &cli) {
    for (int i = 0; i < 300; ++i) {
        auto res = cli.Get("/health");
        if (res && res->status == 200)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace serve_http_test

TEST_CASE("[http] POST /<tool> handles direct JSON, empty body, and passes envelope-shaped body raw on TCP and Unix socket") {
    serve_daemon_server_test::TempDirGuard tmp("raw-env");
    auto sockPath = (tmp.dir / "raw.sock").string();

    slimr::ServeApp app("calc", "1.2.3");
    app.apiVersion = 2;
    app.tool<serve_http_test::AddIn>(
        "add",
        "Add two integers",
        [](const serve_http_test::AddIn &in) {
            return serve_http_test::AddOut{.sum = in.a.value + in.b.value};
        });
    app.registerTool(slimr::ToolDef{
        .name = "echo_raw",
        .description = "Echo raw JSON args",
        .inputSchema = slimr::schema<serve_http_test::EmptyIn>(),
        .handler = [](const slimr::Json &args, const slimr::RequestContext &) {
            return slimr::ServeResult::json(args);
        },
    });

    slimr::ServeHttpOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    opts.socketPath = sockPath;
    slimr::ServeHttpServer srv(app, opts);
    std::string err;
    REQUIRE(srv.start(&err));

    httplib::Client cli("127.0.0.1", srv.boundPort());
    auto sock = serve_http_test::unix_client(sockPath);

    auto res1 = cli.Post("/add", R"({"a":7,"b":5})", "application/json");
    REQUIRE(res1);
    CHECK(res1->status == 200);
    auto j1 = slimr::parse_json<slimr::Json>(res1->body);
    REQUIRE(j1.has_value());
    CHECK((*j1)["sum"].get<int64_t>() == 12);

    const std::string envelopeBody = R"({"input":{"msg":"hi"},"context":{"session":"s-42"}})";
    for (auto *c : {&cli, &sock}) {
        auto res2 = c->Post("/echo_raw", envelopeBody, "application/json");
        REQUIRE(res2);
        CHECK(res2->status == 200);
        auto j2 = slimr::parse_json<slimr::Json>(res2->body);
        REQUIRE(j2.has_value());
        CHECK((*j2)["input"]["msg"].get<std::string>() == "hi");
        CHECK((*j2)["context"]["session"].get<std::string>() == "s-42");
    }

    auto res3 = cli.Post("/echo_raw", "", "application/json");
    REQUIRE(res3);
    CHECK(res3->status == 200);
    auto j3 = slimr::parse_json<slimr::Json>(res3->body);
    REQUIRE(j3.has_value());
    CHECK(j3->empty());

    srv.stop();
}

TEST_CASE("[http] POST /<tool> streams NDJSON progress, log, and result when Accept requests it") {
    slimr::ServeApp app("streamer", "0.1.0");
    app.tool<serve_http_test::EmptyIn>(
        "work",
        "Stream progress",
        [](const serve_http_test::EmptyIn &, const slimr::RequestContext &ctx) {
            ctx.emitProgress(1.0, 2.0, "step 1");
            ctx.emitLog("info", slimr::Json("working Bearer secret-tok"), "job");
            ctx.emitProgress(2.0, 2.0, "step 2");
            return serve_http_test::AddOut{.sum = 99};
        });
    app.tool<serve_http_test::EmptyIn>(
        "fail_stream",
        "Fail during stream",
        [](const serve_http_test::EmptyIn &, const slimr::RequestContext &ctx) -> slimr::ServeResult {
            ctx.emitProgress(0.5, 1.0, "starting");
            return slimr::ServeResult::error("boom", 422);
        });

    slimr::ServeHttpOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    slimr::ServeHttpServer srv(app, opts);
    REQUIRE(srv.start());

    httplib::Client cli("127.0.0.1", srv.boundPort());
    httplib::Headers hdrs{{"Accept", "application/x-ndjson, application/json"}};

    auto res1 = cli.Post("/work", hdrs, "{}", "application/json");
    REQUIRE(res1);
    CHECK(res1->status == 200);
    CHECK(res1->get_header_value("Content-Type").find("application/x-ndjson") != std::string::npos);

    std::vector<slimr::Json> events;
    std::istringstream iss(res1->body);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty())
            continue;
        auto parsed = slimr::parse_json<slimr::Json>(line);
        REQUIRE(parsed.has_value());
        events.push_back(*parsed);
    }
    REQUIRE(events.size() == 4);
    CHECK(events[0]["type"].get<std::string>() == "progress");
    CHECK(events[1]["type"].get<std::string>() == "log");
    CHECK(events[1]["data"].get<std::string>().find("secret-tok") == std::string::npos);
    CHECK(events[2]["type"].get<std::string>() == "progress");
    CHECK(events[3]["type"].get<std::string>() == "result");
    CHECK(events[3]["data"]["sum"].get<int64_t>() == 99);

    auto res2 = cli.Post("/fail_stream", hdrs, "{}", "application/json");
    REQUIRE(res2);
    CHECK(res2->status == 200);
    std::vector<slimr::Json> errEvents;
    std::istringstream iss2(res2->body);
    while (std::getline(iss2, line)) {
        if (line.empty())
            continue;
        auto parsed = slimr::parse_json<slimr::Json>(line);
        REQUIRE(parsed.has_value());
        errEvents.push_back(*parsed);
    }
    REQUIRE(errEvents.size() == 2);
    CHECK(errEvents[0]["type"].get<std::string>() == "progress");
    CHECK(errEvents[1]["type"].get<std::string>() == "error");
    CHECK(errEvents[1]["status"].get<int64_t>() == 422);
    CHECK(errEvents[1]["message"].get<std::string>() == "boom");

    srv.stop();
}

TEST_CASE("[http] GET /health and GET /schema expose metadata and reflected schemas") {
    slimr::ServeApp app("inspector", "2.4.0");
    app.apiVersion = 7;
    app.tool<serve_http_test::AddIn>(
        "add",
        "Add numbers",
        slimr::ToolOptions{.annotations = {.readOnlyHint = true}, .threadSafe = true},
        [](const serve_http_test::AddIn &in) {
            return serve_http_test::AddOut{.sum = in.a.value + in.b.value};
        });
    app.tool<serve_http_test::EmptyIn>(
        "raw",
        "Raw json",
        [](const serve_http_test::EmptyIn &) {
            return slimr::json_obj();
        });

    slimr::ServeHttpOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    slimr::ServeHttpServer srv(app, opts);
    REQUIRE(srv.start());

    httplib::Client cli("127.0.0.1", srv.boundPort());

    auto health = cli.Get("/health");
    REQUIRE(health);
    CHECK(health->status == 200);
    auto hj = slimr::parse_json<slimr::Json>(health->body);
    REQUIRE(hj.has_value());
    CHECK((*hj)["status"].get<std::string>() == "ok");
    CHECK((*hj)["key"].get<std::string>() == "inspector");
    CHECK((*hj)["version"].get<std::string>() == "2.4.0");
    CHECK((*hj)["apiVersion"].get<int64_t>() == 7);

    auto schemaRes = cli.Get("/schema");
    REQUIRE(schemaRes);
    CHECK(schemaRes->status == 200);
    auto sj = slimr::parse_json<slimr::Json>(schemaRes->body);
    REQUIRE(sj.has_value());
    CHECK((*sj)["apiVersion"].get<int64_t>() == 7);
    REQUIRE((*sj)["tools"].contains("add"));
    CHECK_FALSE((*sj)["tools"]["add"].contains("runsLocal"));
    CHECK((*sj)["tools"]["add"]["threadSafe"].get<bool>());
    CHECK((*sj)["tools"]["add"]["inputSchema"]["properties"].contains("a"));
    CHECK((*sj)["tools"]["add"]["outputSchema"]["properties"].contains("sum"));
    REQUIRE((*sj)["tools"].contains("raw"));
    CHECK_FALSE((*sj)["tools"]["raw"].contains("outputSchema"));

    srv.stop();
}

TEST_CASE("[http] customRoutes take precedence over POST /:tool and receive remoteAddr") {
    slimr::ServeApp app("custom", "0.1.0");
    app.tool<serve_http_test::EmptyIn>(
        "override",
        "Should be shadowed by custom route",
        [](const serve_http_test::EmptyIn &) {
            return std::string("tool-hit");
        });

    std::string observedRemoteAddr;
    slimr::ServeHttpOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    opts.customRoutes.push_back(slimr::CustomHttpRoute{
        .method = "POST",
        .path = "/override",
        .handler = [&observedRemoteAddr](const slimr::HttpRouteRequest &req) {
            observedRemoteAddr = req.remoteAddr;
            slimr::Json j = slimr::json_obj();
            j["source"] = "custom_route";
            j["remote"] = req.remoteAddr;
            return slimr::HttpRouteResponse::json(201, j);
        },
    });
    opts.customRoutes.push_back(slimr::CustomHttpRoute{
        .method = "GET",
        .path = "/ping",
        .handler = [](const slimr::HttpRouteRequest &) {
            return slimr::HttpRouteResponse::html(200, "<b>pong</b>");
        },
    });

    slimr::ServeHttpServer srv(app, opts);
    REQUIRE(srv.start());

    httplib::Client cli("127.0.0.1", srv.boundPort());
    auto r1 = cli.Post("/override", "{}", "application/json");
    REQUIRE(r1);
    CHECK(r1->status == 201);
    auto j1 = slimr::parse_json<slimr::Json>(r1->body);
    REQUIRE(j1.has_value());
    CHECK((*j1)["source"].get<std::string>() == "custom_route");
    CHECK(observedRemoteAddr.starts_with("127.0.0.1:"));

    auto r2 = cli.Get("/ping");
    REQUIRE(r2);
    CHECK(r2->status == 200);
    CHECK(r2->body == "<b>pong</b>");

    srv.stop();
}

TEST_CASE("[http] error status code mapping covers 400, 401, 403, 404, 405, 413, 415, 426, 500, 503") {
    slimr::ServeApp app("errors", "0.1.0");
    app.apiVersion = 3;
    app.tool<serve_http_test::AddIn>(
        "add",
        "Add",
        slimr::ToolOptions{.requiredScopes = {"calc:write"}},
        [](const serve_http_test::AddIn &in) {
            return serve_http_test::AddOut{.sum = in.a.value + in.b.value};
        });
    app.tool<serve_http_test::EmptyIn>(
        "crash",
        "Throw exception",
        [](const serve_http_test::EmptyIn &) -> std::string {
            throw std::runtime_error("failed with Bearer tok999");
        });

    bool authUnavailable = false;
    slimr::ServeHttpOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    opts.maxRequestBytes = 256;
    opts.oauth.enabled = true;
    opts.oauth.validateToken = [&authUnavailable](const slimr::TokenValidationContext &ctx) {
        slimr::TokenValidationResult r;
        if (authUnavailable) {
            r.status = slimr::TokenValidationResult::Status::ServiceUnavailable;
            r.errorDescription = "down";
            return r;
        }
        if (ctx.token == "good-rw") {
            r.status = slimr::TokenValidationResult::Status::Valid;
            r.principal.authenticated = true;
            r.principal.subject = "user-1";
            r.principal.audience = std::string(ctx.expectedResource);
            r.principal.scopes = {"calc:write"};
            return r;
        }
        if (ctx.token == "read-only") {
            r.status = slimr::TokenValidationResult::Status::Valid;
            r.principal.authenticated = true;
            r.principal.subject = "user-2";
            r.principal.audience = std::string(ctx.expectedResource);
            r.principal.scopes = {"calc:read"};
            return r;
        }
        r.status = slimr::TokenValidationResult::Status::InvalidToken;
        r.errorDescription = "bad token";
        return r;
    };

    slimr::ServeHttpServer srv(app, opts);
    REQUIRE(srv.start());

    httplib::Client cli("127.0.0.1", srv.boundPort());
    httplib::Headers goodAuth{{"Authorization", "Bearer good-rw"}};

    // 405 Method Not Allowed
    auto r405Tool = cli.Get("/add");
    REQUIRE(r405Tool);
    CHECK(r405Tool->status == 405);
    CHECK(r405Tool->get_header_value("Allow") == "POST");

    auto r405Health = cli.Post("/health", "{}", "application/json");
    REQUIRE(r405Health);
    CHECK(r405Health->status == 405);
    CHECK(r405Health->get_header_value("Allow") == "GET");

    // 426 Upgrade Required (client apiVersion > server apiVersion)
    httplib::Headers newerVer = goodAuth;
    newerVer.emplace("X-Slimr-Api-Version", "4");
    auto r426 = cli.Post("/add", newerVer, R"({"a":1,"b":2})", "application/json");
    REQUIRE(r426);
    CHECK(r426->status == 426);

    // 400 Invalid X-Slimr-Api-Version
    httplib::Headers badVer = goodAuth;
    badVer.emplace("X-Slimr-Api-Version", "abc");
    auto r400Ver = cli.Post("/add", badVer, R"({"a":1,"b":2})", "application/json");
    REQUIRE(r400Ver);
    CHECK(r400Ver->status == 400);

    // 415 Unsupported Media Type
    auto r415 = cli.Post("/add", goodAuth, "a=1", "text/plain");
    REQUIRE(r415);
    CHECK(r415->status == 415);

    // 413 Payload Too Large
    std::string bigBody(512, 'x');
    auto r413 = cli.Post("/add", goodAuth, bigBody, "application/json");
    REQUIRE(r413);
    CHECK(r413->status == 413);

    // 404 Not Found
    auto r404 = cli.Post("/nonexistent", goodAuth, "{}", "application/json");
    REQUIRE(r404);
    CHECK(r404->status == 404);

    // 401 Unauthorized (missing Bearer)
    auto r401 = cli.Post("/add", R"({"a":1,"b":2})", "application/json");
    REQUIRE(r401);
    CHECK(r401->status == 401);
    CHECK(r401->has_header("WWW-Authenticate"));

    // 403 Insufficient Scope
    httplib::Headers roAuth{{"Authorization", "Bearer read-only"}};
    auto r403 = cli.Post("/add", roAuth, R"({"a":1,"b":2})", "application/json");
    REQUIRE(r403);
    CHECK(r403->status == 403);
    CHECK(r403->has_header("WWW-Authenticate"));

    // 400 Malformed JSON and invalid params
    auto r400Json = cli.Post("/add", goodAuth, "{bad", "application/json");
    REQUIRE(r400Json);
    CHECK(r400Json->status == 400);

    auto r400Params = cli.Post("/add", goodAuth, R"({"a":"not-int"})", "application/json");
    REQUIRE(r400Params);
    CHECK(r400Params->status == 400);

    // 500 Internal Error (exception redacted)
    auto r500 = cli.Post("/crash", goodAuth, "{}", "application/json");
    REQUIRE(r500);
    CHECK(r500->status == 500);
    CHECK(r500->body.find("tok999") == std::string::npos);

    // 503 Service Unavailable from auth backend
    authUnavailable = true;
    auto r503 = cli.Post("/add", goodAuth, R"({"a":1,"b":2})", "application/json");
    REQUIRE(r503);
    CHECK(r503->status == 503);

    srv.stop();
}

TEST_CASE("[http] D13 graceful drain finishes in-flight call, rejects new with 503, and closes listener") {
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;

    slimr::ServeApp app("drain", "0.1.0");
    app.tool<serve_http_test::EmptyIn>(
        "slow",
        "Wait for release",
        [&](const serve_http_test::EmptyIn &) {
            {
                std::lock_guard lock(mu);
                entered = true;
            }
            cv.notify_all();
            std::unique_lock lock(mu);
            cv.wait(lock, [&]() { return release; });
            return std::string("finished");
        });

    slimr::ServeHttpOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    opts.shutdownTimeoutMs = 3000;
    slimr::ServeHttpServer srv(app, opts);
    REQUIRE(srv.start());
    uint16_t port = srv.boundPort();

    httplib::Result inflightRes;
    std::thread inflight([&]() {
        httplib::Client cli("127.0.0.1", port);
        inflightRes = cli.Post("/slow", "{}", "application/json");
    });

    {
        std::unique_lock lock(mu);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&]() { return entered; }));
    }

    std::thread stopping([&]() { srv.stop(); });

    // Poll until draining rejects new connections with 503 + Connection: close.
    bool saw503 = false;
    for (int i = 0; i < 40; ++i) {
        httplib::Client probe("127.0.0.1", port);
        auto pr = probe.Get("/health");
        if (pr && pr->status == 503) {
            CHECK(pr->get_header_value("Connection") == "close");
            saw503 = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(saw503);

    {
        std::lock_guard lock(mu);
        release = true;
    }
    cv.notify_all();

    inflight.join();
    stopping.join();

    REQUIRE(inflightRes);
    CHECK(inflightRes->status == 200);
    CHECK(inflightRes->body == "\"finished\"");
    CHECK(srv.boundPort() == 0);
    CHECK_FALSE(serve_http_test::can_connect_loopback(port));
}

TEST_CASE("[http] enable_serve and enable_http attach runner and validate --port") {
    slimr::ServeApp app1("s1", "0.1.0");
    slimr::enable_serve(app1);
    CHECK(app1.hasHttpRunner());
    char arg0[] = "s1";
    char arg1[] = "serve";
    char arg2[] = "--port";
    char arg3[] = "99999";
    char *argv1[] = {arg0, arg1, arg2, arg3};
    CHECK(app1.httpRunner()(app1, 4, argv1) == 2);

    slimr::ServeApp app2("s2", "0.1.0");
    slimr::enable_http(app2);
    CHECK(app2.hasHttpRunner());
}

TEST_CASE("[http] serve --socket serves POST /<tool> and holds a single-instance lock") {
    serve_daemon_server_test::TempDirGuard tmp("ss");
    auto sockPath = (tmp.dir / "serve.sock").string();

    slimr::ServeApp app("sock-serve", "0.1.0");
    app.tool<serve_http_test::AddIn>("add", "Add", [](const serve_http_test::AddIn &in) {
        return serve_http_test::AddOut{.sum = in.a.value + in.b.value};
    });
    slimr::enable_serve(app);

    serve_http_test::ServeRunner runner(app, {"sock-serve", "serve", "--socket", sockPath, "--socket-mode", "0660"});
    auto cli = serve_http_test::unix_client(sockPath);
    REQUIRE(serve_http_test::wait_for_health(cli));

    struct stat st{};
    REQUIRE(::stat(sockPath.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0660);

    auto res = cli.Post("/add", R"({"a":2,"b":3})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    auto j = slimr::parse_json<slimr::Json>(res->body);
    REQUIRE(j.has_value());
    CHECK((*j)["sum"].get<int64_t>() == 5);

    CHECK(serve_http_test::run_serve(app, {"sock-serve", "serve", "--socket", sockPath}) == 1);

    CHECK(runner.stop() == 0);
    CHECK_FALSE(std::filesystem::exists(sockPath));
}

TEST_CASE("[http] TCP and Unix socket answer the same routes; the socket uses the local principal") {
    serve_daemon_server_test::TempDirGuard tmp("du");
    auto sockPath = (tmp.dir / "dual.sock").string();

    slimr::ServeApp app("dual", "0.1.0");
    app.tool<serve_http_test::AddIn>(
        "add",
        "Add",
        slimr::ToolOptions{.requiredScopes = {"calc:write"}},
        [](const serve_http_test::AddIn &in) {
            return serve_http_test::AddOut{.sum = in.a.value + in.b.value};
        });

    slimr::ServeHttpOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    opts.socketPath = sockPath;
    opts.oauth.enabled = true;
    opts.oauth.validateToken = [](const slimr::TokenValidationContext &ctx) {
        slimr::TokenValidationResult r;
        if (ctx.token == "good") {
            r.status = slimr::TokenValidationResult::Status::Valid;
            r.principal.authenticated = true;
            r.principal.audience = std::string(ctx.expectedResource);
            r.principal.scopes = {"calc:write"};
        }
        return r;
    };
    opts.customRoutes.push_back(slimr::CustomHttpRoute{
        .method = "GET",
        .path = "/transport",
        .handler = [](const slimr::HttpRouteRequest &req) {
            return slimr::HttpRouteResponse::html(200, req.unixSocket ? "unix" : "tcp");
        },
    });

    slimr::ServeHttpServer srv(app, opts);
    std::string startErr;
    REQUIRE(srv.start(&startErr));
    REQUIRE(srv.boundPort() > 0);

    httplib::Client tcp("127.0.0.1", srv.boundPort());
    auto sock = serve_http_test::unix_client(sockPath);
    const std::string body = R"({"a":1,"b":2})";

    auto tcpNoAuth = tcp.Post("/add", body, "application/json");
    REQUIRE(tcpNoAuth);
    CHECK(tcpNoAuth->status == 401);
    auto tcpRes = tcp.Post("/add", httplib::Headers{{"Authorization", "Bearer good"}}, body, "application/json");
    REQUIRE(tcpRes);
    CHECK(tcpRes->status == 200);
    auto unixRes = sock.Post("/add", body, "application/json");
    REQUIRE(unixRes);
    CHECK(unixRes->status == 200);
    CHECK(unixRes->body == tcpRes->body);

    auto tcpRoute = tcp.Get("/transport");
    auto unixRoute = sock.Get("/transport");
    REQUIRE(tcpRoute);
    REQUIRE(unixRoute);
    CHECK(tcpRoute->body == "tcp");
    CHECK(unixRoute->body == "unix");

    auto unixSchema = sock.Get("/schema");
    REQUIRE(unixSchema);
    CHECK(unixSchema->status == 200);

    app.localScopes = {"calc:read"};
    auto unixDenied = sock.Post("/add", body, "application/json");
    REQUIRE(unixDenied);
    CHECK(unixDenied->status == 403);
    CHECK_FALSE(unixDenied->has_header("WWW-Authenticate"));

    srv.stop();
    CHECK_FALSE(std::filesystem::exists(sockPath));
}

TEST_CASE("[http] onServe runs under serve, stops on shutdown, and a failure exits nonzero") {
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    std::mutex mu;
    std::condition_variable_any cv;

    slimr::ServeApp app("on-serve", "0.1.0");
    app.onServe = [&](std::stop_token st) {
        started.store(true);
        std::unique_lock lk(mu);
        cv.wait(lk, st, [&st] { return st.stop_requested(); });
        stopped.store(true);
    };
    slimr::enable_serve(app, {.host = "127.0.0.1", .port = 0});

    serve_http_test::ServeRunner runner(app, {"on-serve", "serve"});
    for (int i = 0; i < 200 && !started.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(started.load());
    CHECK_FALSE(stopped.load());
    CHECK(runner.stop() == 0);
    CHECK(stopped.load());

    slimr::ServeApp errApp("on-serve-err", "0.1.0");
    errApp.onServe = [](std::stop_token) { throw std::runtime_error("boom Bearer secret123"); };
    slimr::enable_serve(errApp, {.host = "127.0.0.1", .port = 0});
    CHECK(serve_http_test::run_serve(errApp, {"on-serve-err", "serve"}) == 1);
}

TEST_CASE("[http] serve rejects bad socket flags and requires a listener") {
    slimr::ServeApp app("flags", "0.1.0");
    slimr::enable_serve(app);
    CHECK(serve_http_test::run_serve(app, {"flags", "serve", "--socket"}) == 2);
    CHECK(serve_http_test::run_serve(app, {"flags", "serve", "--socket", std::string(200, 'a')}) == 2);
    CHECK(serve_http_test::run_serve(app, {"flags", "serve", "--socket-mode", "999"}) == 2);
    CHECK(serve_http_test::run_serve(app, {"flags", "serve", "--socket-mode", "rw"}) == 2);
    CHECK(serve_http_test::run_serve(app, {"flags", "serve", "--socket-group", ""}) == 2);
    CHECK(serve_http_test::run_serve(app, {"flags", "http", "--host", "localhost"}) == 2);
    CHECK(serve_http_test::run_serve(app, {"flags", "serve"}) == 1);
}

TEST_CASE("[http] GET /openapi.json serves unauthenticated OpenAPI 3.1.0 and 405s other methods") {
    slimr::ServeApp app("oa-http", "1.0.0");
    app.tool<serve_http_test::AddIn>("add", "Add", [](const serve_http_test::AddIn &in) {
        return serve_http_test::AddOut{.sum = in.a.value + in.b.value};
    });

    // TCP with OAuth enabled: GET /openapi.json is unauthenticated and includes servers[0].url + bearerAuth
    {
        slimr::ServeHttpOptions opts;
        opts.host = "127.0.0.1";
        opts.port = 0;
        opts.oauth.enabled = true;
        opts.oauth.validateToken = [](const slimr::TokenValidationContext &) {
            return slimr::TokenValidationResult{};
        };
        slimr::enable_serve(app, opts);

        slimr::ServeHttpServer srv(app, opts);
        REQUIRE(srv.start());
        httplib::Client tcp("127.0.0.1", srv.boundPort());

        auto getRes = tcp.Get("/openapi.json");
        REQUIRE(getRes);
        CHECK(getRes->status == 200);
        auto spec = slimr::parse_json<slimr::Json>(getRes->body);
        REQUIRE(spec.has_value());
        CHECK((*spec)["openapi"].get<std::string>() == "3.1.0");
        CHECK((*spec)["servers"][0]["url"].get<std::string>() == srv.baseUrl());
        CHECK((*spec)["components"]["securitySchemes"]["bearerAuth"]["scheme"].get<std::string>() == "bearer");
        CHECK((*spec)["paths"]["/add"]["post"]["responses"].contains("401"));

        auto check405 = [](const httplib::Result &res) {
            REQUIRE(res);
            CHECK(res->status == 405);
            CHECK(res->get_header_value("Allow") == "GET");
        };
        check405(tcp.Post("/openapi.json", "{}", "application/json"));
        check405(tcp.Put("/openapi.json", "{}", "application/json"));
        check405(tcp.Delete("/openapi.json"));
        check405(tcp.Patch("/openapi.json", "{}", "application/json"));
        srv.stop();
    }

    // Unix-socket-only server omits servers
    {
        serve_daemon_server_test::TempDirGuard tmp("oa-sock");
        auto sockPath = (tmp.dir / "oa.sock").string();
        slimr::ServeHttpOptions opts;
        opts.socketPath = sockPath;
        slimr::ServeHttpServer srv(app, opts);
        REQUIRE(srv.start());
        auto sock = serve_http_test::unix_client(sockPath);

        auto getRes = sock.Get("/openapi.json");
        REQUIRE(getRes);
        CHECK(getRes->status == 200);
        auto spec = slimr::parse_json<slimr::Json>(getRes->body);
        REQUIRE(spec.has_value());
        CHECK((*spec)["openapi"].get<std::string>() == "3.1.0");
        CHECK_FALSE(spec->contains("servers"));
        CHECK((*spec)["paths"].contains("/add"));
        srv.stop();
    }
}

TEST_CASE("[http] default host is 127.0.0.1 when --host is omitted") {
    slimr::ServeHttpOptions defaultOpts;
    CHECK(defaultOpts.host == "127.0.0.1");

    slimr::ServeApp app("loopback", "0.1.0");
    slimr::ServeHttpServer srv(app, {.port = 0});
    REQUIRE(srv.start());
    CHECK(srv.baseUrl().starts_with("http://127.0.0.1:"));
    srv.stop();

    std::atomic<bool> started{false};
    app.onServe = [&](std::stop_token) { started.store(true); };
    slimr::enable_serve(app, {.port = 0});
    std::ostringstream capturedErr;
    auto *oldBuf = std::cerr.rdbuf(capturedErr.rdbuf());
    {
        serve_http_test::ServeRunner runner(app, {"loopback", "serve"});
        for (int i = 0; i < 200 && !started.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        REQUIRE(started.load());
        CHECK(runner.stop() == 0);
    }
    std::cerr.rdbuf(oldBuf);
    CHECK(capturedErr.str().find("listening on http://127.0.0.1:") != std::string::npos);
}

TEST_CASE("[http] toolRoutes=false returns 404 on tool routes while keeping /health and customRoutes") {
    slimr::ServeApp app("no-tools", "0.1.0");
    app.tool<serve_http_test::AddIn>("add", "Add", [](const serve_http_test::AddIn &in) {
        return serve_http_test::AddOut{.sum = in.a.value + in.b.value};
    });

    slimr::ServeHttpOptions opts;
    opts.port = 0;
    opts.toolRoutes = false;
    opts.customRoutes.push_back(slimr::CustomHttpRoute{
        .method = "POST",
        .path = "/mcp",
        .handler = [](const slimr::HttpRouteRequest &) {
            return slimr::HttpRouteResponse::json(200, slimr::json_obj());
        },
    });

    slimr::ServeHttpServer srv(app, opts);
    REQUIRE(srv.start());
    httplib::Client cli("127.0.0.1", srv.boundPort());

    auto health = cli.Get("/health");
    REQUIRE(health);
    CHECK(health->status == 200);

    auto mcp = cli.Post("/mcp", "{}", "application/json");
    REQUIRE(mcp);
    CHECK(mcp->status == 200);

    auto oa = cli.Get("/openapi.json");
    REQUIRE(oa);
    CHECK(oa->status == 404);

    auto sc = cli.Get("/schema");
    REQUIRE(sc);
    CHECK(sc->status == 404);

    auto tool = cli.Post("/add", R"({"a":1,"b":2})", "application/json");
    REQUIRE(tool);
    CHECK(tool->status == 404);

    srv.stop();
}

TEST_CASE("[http] toolsPrefix mounts /schema, /openapi.json, and /:tool under prefix and 404s at root") {
    slimr::ServeApp app("prefixed", "0.1.0");
    app.tool<serve_http_test::AddIn>("add", "Add", [](const serve_http_test::AddIn &in) {
        return serve_http_test::AddOut{.sum = in.a.value + in.b.value};
    });

    slimr::ServeHttpOptions opts;
    opts.port = 0;
    opts.toolsPrefix = "/tools";

    slimr::ServeHttpServer srv(app, opts);
    REQUIRE(srv.start());
    httplib::Client cli("127.0.0.1", srv.boundPort());

    // Root tool routes return 404
    CHECK(cli.Get("/openapi.json")->status == 404);
    CHECK(cli.Get("/schema")->status == 404);
    CHECK(cli.Post("/add", R"({"a":1,"b":2})", "application/json")->status == 404);

    // Prefixed routes work
    auto oa = cli.Get("/tools/openapi.json");
    REQUIRE(oa);
    CHECK(oa->status == 200);
    auto spec = slimr::parse_json<slimr::Json>(oa->body);
    REQUIRE(spec.has_value());
    CHECK((*spec)["paths"].contains("/tools/add"));
    CHECK_FALSE((*spec)["paths"].contains("/add"));

    auto sc = cli.Get("/tools/schema");
    REQUIRE(sc);
    CHECK(sc->status == 200);
    auto sj = slimr::parse_json<slimr::Json>(sc->body);
    REQUIRE(sj.has_value());
    CHECK((*sj)["tools"].contains("add"));

    auto res = cli.Post("/tools/add", R"({"a":3,"b":4})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    auto j = slimr::parse_json<slimr::Json>(res->body);
    REQUIRE(j.has_value());
    CHECK((*j)["sum"].get<int64_t>() == 7);

    srv.stop();
}

TEST_CASE("[http] oauth.expectedResource overrides baseUrl in validateToken") {
    slimr::ServeApp app("oauth-res", "0.1.0");
    app.tool<serve_http_test::EmptyIn>("ping", "Ping", [](const serve_http_test::EmptyIn &) {
        return std::string("pong");
    });

    std::string seenExpectedResource;
    slimr::ServeHttpOptions opts;
    opts.port = 0;
    opts.oauth.enabled = true;
    opts.oauth.expectedResource = "https://example.test/mcp";
    opts.oauth.validateToken = [&seenExpectedResource](const slimr::TokenValidationContext &ctx) {
        seenExpectedResource = std::string(ctx.expectedResource);
        slimr::TokenValidationResult r;
        r.status = slimr::TokenValidationResult::Status::Valid;
        r.principal.authenticated = true;
        r.principal.subject = "u1";
        r.principal.audience = std::string(ctx.expectedResource);
        return r;
    };

    slimr::ServeHttpServer srv(app, opts);
    REQUIRE(srv.start());
    httplib::Client cli("127.0.0.1", srv.boundPort());

    auto res = cli.Post("/ping", httplib::Headers{{"Authorization", "Bearer tok"}}, "{}", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(seenExpectedResource == "https://example.test/mcp");

    srv.stop();
}

TEST_CASE("[http] non-opted ServeHttpServer rejects application/yaml, form-urlencoded, and missing Content-Type with 415") {
    serve_daemon_server_test::TempDirGuard tmp("no-yaml");
    auto sockPath = (tmp.dir / "no-yaml.sock").string();

    slimr::ServeApp app("no-yaml", "0.1.0");
    app.tool<serve_http_test::AddIn>("add", "Add", [](const serve_http_test::AddIn &in) {
        return serve_http_test::AddOut{.sum = in.a.value + in.b.value};
    });

    slimr::ServeHttpOptions opts;
    opts.port = 0;
    opts.socketPath = sockPath;
    slimr::ServeHttpServer srv(app, opts);
    REQUIRE(srv.start());

    httplib::Client tcp("127.0.0.1", srv.boundPort());
    auto sock = serve_http_test::unix_client(sockPath);

    for (auto *c : {&tcp, &sock}) {
        auto rYaml = c->Post("/add", "a: 1\nb: 2\n", "application/yaml");
        REQUIRE(rYaml);
        CHECK(rYaml->status == 415);

        auto rForm = c->Post("/add", R"({"a":1,"b":2})", "application/x-www-form-urlencoded");
        REQUIRE(rForm);
        CHECK(rForm->status == 415);

        // Post without Content-Type and non-empty body
        httplib::Request rawReq;
        rawReq.method = "POST";
        rawReq.path = "/add";
        rawReq.body = R"({"a":1,"b":2})";
        auto rMissing = c->send(rawReq);
        REQUIRE(rMissing);
        CHECK(rMissing->status == 415);
    }

    auto oa = tcp.Get("/openapi.json");
    REQUIRE(oa);
    CHECK(oa->status == 200);
    auto spec = slimr::parse_json<slimr::Json>(oa->body);
    REQUIRE(spec.has_value());
    CHECK_FALSE((*spec)["info"].contains("description"));

    srv.stop();
}

TEST_CASE("[http] ServeHttpOptions.yaml hook supports detection, fallback, Accept override, and OpenAPI note") {
    serve_daemon_server_test::TempDirGuard tmp("yaml-hook");
    auto sockPath = (tmp.dir / "yaml-hook.sock").string();

    slimr::ServeApp app("yaml-hook", "0.1.0");
    app.tool<serve_http_test::AddIn>("add", "Add", [](const serve_http_test::AddIn &in) {
        return serve_http_test::AddOut{.sum = in.a.value + in.b.value};
    });

    slimr::ServeHttpOptions opts;
    opts.port = 0;
    opts.socketPath = sockPath;
    opts.yaml = slimr::YamlCodec{
        .parse = [](std::string_view sv) -> std::expected<slimr::Json, std::string> {
            if (sv == "{a: 1, b: 2}") {
                slimr::Json j = slimr::json_obj();
                j["a"] = 1;
                j["b"] = 2;
                return j;
            }
            if (sv == "a: 3\nb: 4\n" || sv == "  a: 3\nb: 4\n") {
                slimr::Json j = slimr::json_obj();
                j["a"] = 3;
                j["b"] = 4;
                return j;
            }
            return std::unexpected("bad yaml");
        },
        .emit = [](const slimr::Json &j) -> std::string {
            return "yaml:" + slimr::write_json(j);
        },
    };

    slimr::ServeHttpServer srv(app, opts);
    REQUIRE(srv.start());

    httplib::Client tcp("127.0.0.1", srv.boundPort());
    auto sock = serve_http_test::unix_client(sockPath);

    // Explicit application/yaml over TCP and Unix socket -> YAML out
    for (auto *c : {&tcp, &sock}) {
        auto r = c->Post("/add", "a: 3\nb: 4\n", "application/yaml");
        REQUIRE(r);
        CHECK(r->status == 200);
        CHECK(r->get_header_value("Content-Type") == "application/yaml");
        CHECK(r->body == R"(yaml:{"sum":7})");
    }

    // Explicit application/json with Accept: application/yaml -> YAML out
    auto rOverrideYaml = tcp.Post("/add",
                                  httplib::Headers{{"Accept", "application/yaml"}},
                                  R"({"a":2,"b":5})",
                                  "application/json");
    REQUIRE(rOverrideYaml);
    CHECK(rOverrideYaml->status == 200);
    CHECK(rOverrideYaml->get_header_value("Content-Type") == "application/yaml");
    CHECK(rOverrideYaml->body == R"(yaml:{"sum":7})");

    // Accept with both application/json and application/yaml -> JSON wins (Q2)
    auto rBothAccept = tcp.Post("/add",
                                httplib::Headers{{"Accept", "application/json, application/yaml"}},
                                "a: 3\nb: 4\n",
                                "application/yaml");
    REQUIRE(rBothAccept);
    CHECK(rBothAccept->status == 200);
    CHECK(rBothAccept->get_header_value("Content-Type") == "application/json");
    CHECK(rBothAccept->body == R"({"sum":7})");

    // BOM + whitespace + '{' with non-explicit Content-Type -> JSON
    auto rBomJson = tcp.Post("/add", "\xEF\xBB\xBF  {\"a\":4,\"b\":6}", "application/x-www-form-urlencoded");
    REQUIRE(rBomJson);
    CHECK(rBomJson->status == 200);
    CHECK(rBomJson->get_header_value("Content-Type") == "application/json");
    CHECK(rBomJson->body == R"({"sum":10})");

    // Leading '{' failing JSON falls back to YAML
    auto rFallback = tcp.Post("/add", "{a: 1, b: 2}", "application/x-www-form-urlencoded");
    REQUIRE(rFallback);
    CHECK(rFallback->status == 200);
    CHECK(rFallback->get_header_value("Content-Type") == "application/yaml");
    CHECK(rFallback->body == R"(yaml:{"sum":3})");

    // Leading '{' failing both reports JSON error
    auto rBothFail = tcp.Post("/add", "{bad", "text/plain");
    REQUIRE(rBothFail);
    CHECK(rBothFail->status == 400);
    CHECK(rBothFail->get_header_value("Content-Type") == "application/json");
    CHECK(rBothFail->body.find("invalid_json") != std::string::npos);

    // Non-'{'/'[' with BOM falls through to YAML (and YAML error on failure)
    auto rBomYaml = tcp.Post("/add", "\xEF\xBB\xBF  a: 3\nb: 4\n", "application/x-www-form-urlencoded");
    REQUIRE(rBomYaml);
    CHECK(rBomYaml->status == 200);
    CHECK(rBomYaml->get_header_value("Content-Type") == "application/yaml");
    CHECK(rBomYaml->body == R"(yaml:{"sum":7})");

    auto rBadYaml = tcp.Post("/add", "not_valid: [", "text/plain");
    REQUIRE(rBadYaml);
    CHECK(rBadYaml->status == 400);
    CHECK(rBadYaml->get_header_value("Content-Type") == "application/yaml");
    CHECK(rBadYaml->body.find("invalid_yaml") != std::string::npos);

    // /openapi.json includes YAML note
    auto oa = tcp.Get("/openapi.json");
    REQUIRE(oa);
    CHECK(oa->status == 200);
    auto spec = slimr::parse_json<slimr::Json>(oa->body);
    REQUIRE(spec.has_value());
    CHECK((*spec)["info"]["description"].get<std::string>() ==
          "Endpoints also accept and return application/yaml.");

    srv.stop();
}
