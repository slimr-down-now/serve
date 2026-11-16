#pragma once
#include "serve.hpp"

#include <httplib.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <utility>

namespace slimr {

inline constexpr int kRelayExitUnavailable = 69;
inline constexpr int kRelayExitInterrupted = 130;

namespace relay_detail {

inline std::atomic<bool> g_relay_interrupted{false};

inline void handle_relay_signal(int) {
    g_relay_interrupted.store(true, std::memory_order_release);
}

struct SignalGuard {
    using HandlerFn = void (*)(int);
    HandlerFn prevInt{SIG_DFL};
    HandlerFn prevTerm{SIG_DFL};

    SignalGuard() {
        g_relay_interrupted.store(false, std::memory_order_release);
        prevInt = std::signal(SIGINT, handle_relay_signal);
        prevTerm = std::signal(SIGTERM, handle_relay_signal);
    }

    SignalGuard(const SignalGuard &) = delete;
    SignalGuard &operator=(const SignalGuard &) = delete;

    ~SignalGuard() {
        std::signal(SIGINT, prevInt);
        std::signal(SIGTERM, prevTerm);
    }
};

struct CancelWatcher {
    httplib::Client &client;
    CancellationToken token;
    uint64_t cancelId{0};
    std::mutex mu;
    std::condition_variable cv;
    bool done{false};
    std::thread watcher;

    CancelWatcher(httplib::Client &cli, CancellationToken tok)
        : client(cli), token(std::move(tok)) {
        if (token.canBeCancelled()) {
            cancelId = token.onCancel([this]() {
                client.stop();
                cv.notify_all();
            });
        }
        watcher = std::thread([this]() {
            std::unique_lock lock(mu);
            while (!done) {
                if (g_relay_interrupted.load(std::memory_order_acquire) ||
                    token.isCancelled()) {
                    lock.unlock();
                    client.stop();
                    return;
                }
                cv.wait_for(lock, std::chrono::milliseconds(20), [this]() {
                    return done || g_relay_interrupted.load(std::memory_order_acquire) ||
                           token.isCancelled();
                });
            }
        });
    }

    CancelWatcher(const CancelWatcher &) = delete;
    CancelWatcher &operator=(const CancelWatcher &) = delete;

    ~CancelWatcher() {
        {
            std::lock_guard lock(mu);
            done = true;
        }
        cv.notify_all();
        if (watcher.joinable()) {
            watcher.join();
        }
        if (cancelId != 0) {
            token.removeOnCancel(cancelId);
        }
    }
};

inline std::string to_upper_env_prefix(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    for (unsigned char c : key) {
        if (c == '-') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(std::toupper(c)));
        }
    }
    return out;
}

inline std::string effective_remote_env_var(const ServeApp &app) {
    if (!app.remoteEnvVar.empty()) {
        return app.remoteEnvVar;
    }
    if (app.key.empty()) {
        return {};
    }
    return to_upper_env_prefix(app.key) + "_REMOTE";
}

inline std::string effective_bearer_env_var(const ServeApp &app) {
    if (app.key.empty()) {
        return {};
    }
    return to_upper_env_prefix(app.key) + "_BEARER";
}

inline std::optional<std::string> find_flag_value(int argc, char *argv[], std::string_view flag) {
    if (argc <= 0 || argv == nullptr) {
        return std::nullopt;
    }
    std::string eqPrefix = std::string(flag) + "=";
    std::optional<std::string> result;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        std::string_view arg(argv[i]);
        if (arg == flag) {
            if (i + 1 < argc && argv[i + 1] != nullptr) {
                result = std::string(argv[i + 1]);
                ++i;
            }
        } else if (arg.starts_with(eqPrefix)) {
            result = std::string(arg.substr(eqPrefix.size()));
        }
    }
    return result;
}

inline std::string resolve_remote_target(const ServeApp &app, int argc, char *argv[]) {
    if (auto flagVal = find_flag_value(argc, argv, "--remote"); flagVal && !flagVal->empty()) {
        return *flagVal;
    }
    std::string envName = effective_remote_env_var(app);
    if (!envName.empty()) {
        if (const char *val = std::getenv(envName.c_str()); val != nullptr && val[0] != '\0') {
            return std::string(val);
        }
    }
    return app.remote;
}

inline bool resolve_bearer_token(const ServeApp &app,
                                 int argc,
                                 char *argv[],
                                 std::optional<std::string> &outToken,
                                 std::string &outError) {
    outToken.reset();
    if (auto flagEnv = find_flag_value(argc, argv, "--bearer-env")) {
        if (flagEnv->empty()) {
            outError = "Empty --bearer-env variable name";
            return false;
        }
        const char *val = std::getenv(flagEnv->c_str());
        if (val == nullptr || val[0] == '\0') {
            outError = "Missing bearer token in environment variable: " + *flagEnv;
            return false;
        }
        outToken = std::string(val);
        return true;
    }
    std::string defaultEnv = effective_bearer_env_var(app);
    if (!defaultEnv.empty()) {
        if (const char *val = std::getenv(defaultEnv.c_str()); val != nullptr && val[0] != '\0') {
            outToken = std::string(val);
        }
    }
    return true;
}

struct ParsedRemoteTarget {
    bool isUnix{false};
    std::string socketPath;
    std::string schemeHostPort;
    std::string basePath;
};

inline bool parse_remote_target(std::string_view target, ParsedRemoteTarget &out, std::string &outError) {
    if (target.empty()) {
        outError = "Empty remote target";
        return false;
    }
    if (target.starts_with("unix:")) {
        std::string_view sock = target.substr(5);
        if (sock.empty()) {
            outError = "Invalid unix remote target: missing socket path";
            return false;
        }
        out = ParsedRemoteTarget{};
        out.isUnix = true;
        out.socketPath = std::string(sock);
        return true;
    }
    if (target.starts_with("http://") || target.starts_with("https://")) {
        size_t schemeEnd = target.find("://");
        size_t pathStart = target.find('/', schemeEnd + 3);
        out = ParsedRemoteTarget{};
        if (pathStart == std::string_view::npos) {
            out.schemeHostPort = std::string(target);
        } else {
            out.schemeHostPort = std::string(target.substr(0, pathStart));
            std::string_view bp = target.substr(pathStart);
            while (bp.size() > 1 && bp.back() == '/') {
                bp.remove_suffix(1);
            }
            if (bp != "/") {
                out.basePath = std::string(bp);
            }
        }
        return true;
    }
    outError = "Invalid remote target (expected unix:<path> or http(s)://...): " + std::string(target);
    return false;
}

inline bool is_unreachable_error(httplib::Error err) noexcept {
    return err == httplib::Error::Connection ||
           err == httplib::Error::ConnectionTimeout ||
           err == httplib::Error::ProxyConnection ||
           err == httplib::Error::SSLConnection;
}

struct NdjsonState {
    bool isNdjson{false};
    int status{0};
    std::string lineBuf;
    std::string plainBody;
    bool hasResult{false};
    bool hasError{false};
    int errorStatus{0};
    std::string errorMessage;
};

inline void process_ndjson_line(std::string_view line,
                                NdjsonState &state,
                                std::ostream &out,
                                std::ostream &err) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    if (line.empty()) {
        return;
    }
    auto parsed = parse_json<Json>(line);
    if (!parsed || !parsed->is_object()) {
        err << line << "\n";
        err.flush();
        return;
    }
    const Json &ev = *parsed;
    std::string type;
    if (ev.contains("type") && ev["type"].is_string()) {
        type = ev["type"].get<std::string>();
    }
    if (type == "progress" || type == "log") {
        err << slimr::write_json(ev) << "\n";
        err.flush();
        return;
    }
    if (type == "result") {
        state.hasResult = true;
        if (ev.contains("data")) {
            out << slimr::write_json(ev["data"]) << "\n";
        } else {
            out << "{}\n";
        }
        out.flush();
        return;
    }
    if (type == "error") {
        state.hasError = true;
        if (ev.contains("status") && ev["status"].is_number()) {
            state.errorStatus = static_cast<int>(ev["status"].as<long long>());
        }
        if (ev.contains("message") && ev["message"].is_string()) {
            state.errorMessage = ev["message"].get<std::string>();
        } else {
            state.errorMessage = slimr::write_json(ev);
        }
        return;
    }
    err << slimr::write_json(ev) << "\n";
    err.flush();
}

} // namespace relay_detail

/** Forwards a CLI tool invocation to a running slimr-serve daemon over Unix socket or HTTP. */
inline int run_cli_relay(ServeApp &app,
                         const ToolDef &def,
                         const Json &args,
                         const RequestContext &ctx,
                         int argc,
                         char *argv[],
                         std::ostream &out = std::cout,
                         std::ostream &err = std::cerr) {
    std::string target = relay_detail::resolve_remote_target(app, argc, argv);
    if (target.empty()) {
        err << "No remote target configured\n";
        return 1;
    }

    relay_detail::ParsedRemoteTarget parsedTarget;
    std::string parseErr;
    if (!relay_detail::parse_remote_target(target, parsedTarget, parseErr)) {
        err << parseErr << "\n";
        return 1;
    }

    std::optional<std::string> bearerToken;
    std::string bearerErr;
    if (!relay_detail::resolve_bearer_token(app, argc, argv, bearerToken, bearerErr)) {
        err << bearerErr << "\n";
        return 1;
    }

    Json payload = args;
    if (ctx.session.has_value()) {
        payload = json_obj();
        payload["input"] = args;
        Json c = json_obj();
        c["session"] = *ctx.session;
        payload["context"] = std::move(c);
    }

    httplib::Client cli = parsedTarget.isUnix
                              ? httplib::Client(parsedTarget.socketPath)
                              : httplib::Client(parsedTarget.schemeHostPort);
    if (parsedTarget.isUnix) {
        cli.set_address_family(AF_UNIX);
    }
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(300, 0);
    cli.set_write_timeout(30, 0);

    httplib::Request req;
    req.method = "POST";
    req.path = parsedTarget.basePath + "/" + def.name;
    req.body = slimr::write_json(payload);
    req.headers.emplace("Content-Type", "application/json");
    req.headers.emplace("Accept", "application/x-ndjson, application/json");
    req.headers.emplace("X-Slimr-Api-Version", std::to_string(app.apiVersion));
    if (bearerToken.has_value()) {
        req.headers.emplace("Authorization", "Bearer " + *bearerToken);
    }

    relay_detail::NdjsonState ndjson;
    req.response_handler = [&ndjson](const httplib::Response &resp) -> bool {
        ndjson.status = resp.status;
        std::string ct = serve_detail::ascii_lower(resp.get_header_value("Content-Type"));
        ndjson.isNdjson = (resp.status == 200 && ct.find("application/x-ndjson") != std::string::npos);
        return true;
    };
    req.content_receiver = [&ndjson, &ctx, &out, &err](const char *data, size_t len, uint64_t, uint64_t) -> bool {
        if (relay_detail::g_relay_interrupted.load(std::memory_order_acquire) ||
            ctx.cancellation.isCancelled()) {
            return false;
        }
        if (!ndjson.isNdjson) {
            ndjson.plainBody.append(data, len);
            return true;
        }
        ndjson.lineBuf.append(data, len);
        size_t nlPos = 0;
        while ((nlPos = ndjson.lineBuf.find('\n')) != std::string::npos) {
            std::string_view line(ndjson.lineBuf.data(), nlPos);
            relay_detail::process_ndjson_line(line, ndjson, out, err);
            ndjson.lineBuf.erase(0, nlPos + 1);
        }
        return true;
    };

    httplib::Result res = [&]() {
        relay_detail::SignalGuard sigGuard;
        relay_detail::CancelWatcher watcher(cli, ctx.cancellation);
        return cli.send(req);
    }();

    if (relay_detail::g_relay_interrupted.load(std::memory_order_acquire)) {
        err << "Cancelled\n";
        return kRelayExitInterrupted;
    }
    if (ctx.cancellation.isCancelled()) {
        err << "Cancelled\n";
        return 1;
    }

    if (!res) {
        httplib::Error httpErr = res.error();
        if (relay_detail::is_unreachable_error(httpErr)) {
            std::string prog = app.key.empty() ? "app" : app.key;
            err << "Daemon unreachable at " << target << "\n";
            err << "Hint: brew services start " << prog << "\n";
            return kRelayExitUnavailable;
        }
        err << "Remote request failed: " << httplib::to_string(httpErr) << "\n";
        return 1;
    }

    if (res->status == 426) {
        std::string msg = "daemon older than client; restart it";
        if (auto parsedErr = parse_json<Json>(ndjson.plainBody);
            parsedErr && parsedErr->is_object() && parsedErr->contains("message") &&
            (*parsedErr)["message"].is_string()) {
            msg = (*parsedErr)["message"].get<std::string>();
        }
        err << "API version mismatch (426): " << msg << "\n";
        return 1;
    }

    if (res->status >= 400) {
        std::string msg;
        if (auto parsedErr = parse_json<Json>(ndjson.plainBody); parsedErr && parsedErr->is_object()) {
            if (parsedErr->contains("message") && (*parsedErr)["message"].is_string()) {
                msg = (*parsedErr)["message"].get<std::string>();
            } else if (parsedErr->contains("error_description") && (*parsedErr)["error_description"].is_string()) {
                msg = (*parsedErr)["error_description"].get<std::string>();
            } else if (parsedErr->contains("error") && (*parsedErr)["error"].is_string()) {
                msg = (*parsedErr)["error"].get<std::string>();
            }
        }
        if (msg.empty()) {
            msg = ndjson.plainBody.empty() ? ("HTTP " + std::to_string(res->status)) : ndjson.plainBody;
        }
        err << redact_sensitive(msg) << "\n";
        return 1;
    }

    if (ndjson.isNdjson) {
        if (!ndjson.lineBuf.empty()) {
            relay_detail::process_ndjson_line(ndjson.lineBuf, ndjson, out, err);
            ndjson.lineBuf.clear();
        }
        if (ndjson.hasError) {
            err << redact_sensitive(ndjson.errorMessage) << "\n";
            return 1;
        }
        return 0;
    }

    std::string_view outView(ndjson.plainBody);
    while (!outView.empty() && (outView.back() == '\n' || outView.back() == '\r')) {
        outView.remove_suffix(1);
    }
    out << outView << "\n";
    return 0;
}

/** Registers run_cli_relay as the remote relay handler on app. */
inline void enable_cli_relay(ServeApp &app) {
    app.setRemoteRelay([](ServeApp &a,
                          const ToolDef &def,
                          const Json &args,
                          const RequestContext &ctx,
                          int argc,
                          char **argv) -> int {
        return run_cli_relay(a, def, args, ctx, argc, argv);
    });
}

} // namespace slimr
