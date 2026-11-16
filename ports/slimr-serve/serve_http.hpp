#pragma once
#include "serve.hpp"
#include "serve_daemon.hpp"

#include <httplib.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace slimr {

#ifndef SLIMR_SERVE_HAS_AUTH
// Fallback auth/route types for standalone build before 15-C0-serve-int merges serve_auth.hpp.
struct HttpRouteRequest {
    std::string method;
    std::string path;
    std::string rawQuery;
    std::map<std::string, std::string> queryParams;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string remoteAddr;

    std::optional<std::string> header(std::string_view name) const {
        auto it = headers.find(serve_detail::ascii_lower(name));
        if (it == headers.end())
            return std::nullopt;
        return it->second;
    }

    std::optional<std::string> query(std::string_view name) const {
        auto it = queryParams.find(std::string(name));
        if (it == queryParams.end())
            return std::nullopt;
        return it->second;
    }
};

struct HttpRouteResponse {
    int status{200};
    std::string contentType{"application/json"};
    std::map<std::string, std::string> headers;
    std::string body;

    static HttpRouteResponse json(int statusCode, const Json &value) {
        HttpRouteResponse r;
        r.status = statusCode;
        r.contentType = "application/json";
        r.body = slimr::write_json(value);
        return r;
    }

    static HttpRouteResponse html(int statusCode, std::string htmlBody) {
        HttpRouteResponse r;
        r.status = statusCode;
        r.contentType = "text/html; charset=utf-8";
        r.body = std::move(htmlBody);
        return r;
    }

    static HttpRouteResponse redirect(std::string location, int statusCode = 302) {
        HttpRouteResponse r;
        r.status = statusCode;
        r.contentType = "text/plain; charset=utf-8";
        r.headers["Location"] = std::move(location);
        return r;
    }
};

struct CustomHttpRoute {
    std::string method;
    std::string path;
    bool enforceSameOrigin{true};
    std::function<HttpRouteResponse(const HttpRouteRequest &)> handler;
};

struct OAuthResourceMetadata {
    std::string resource;
    std::vector<std::string> authorizationServers;
    std::vector<std::string> scopesSupported;
    std::vector<std::string> bearerMethodsSupported{"header"};
    std::string resourceName;
    std::string resourceDocumentation;
};

struct TokenValidationContext {
    std::string_view token;
    std::string_view expectedResource;
    const HttpRouteRequest &request;
};

struct TokenValidationResult {
    enum class Status {
        Valid,
        InvalidToken,
        InsufficientScope,
        ServiceUnavailable
    };

    Status status{Status::InvalidToken};
    Principal principal;
    std::string errorDescription;
    std::vector<std::string> requiredScopes;
};

using TokenValidatorFn = std::function<TokenValidationResult(const TokenValidationContext &)>;

struct OAuthResourceConfig {
    bool enabled{false};
    bool protectDiscover{true};
    OAuthResourceMetadata metadata;
    std::vector<std::string> expectedIssuers;
    std::vector<std::string> defaultScopes;
    std::string metadataPath{"/.well-known/oauth-protected-resource"};
    TokenValidatorFn validateToken;
};

namespace serve_detail {

inline std::optional<std::string> extract_bearer_token(std::string_view authHeader) {
    while (!authHeader.empty() && authHeader.front() == ' ')
        authHeader.remove_prefix(1);
    while (!authHeader.empty() && authHeader.back() == ' ')
        authHeader.remove_suffix(1);
    if (authHeader.size() < 8)
        return std::nullopt;
    if (ascii_lower(authHeader.substr(0, 7)) != "bearer ")
        return std::nullopt;
    std::string_view token = authHeader.substr(7);
    while (!token.empty() && token.front() == ' ')
        token.remove_prefix(1);
    if (token.empty())
        return std::nullopt;
    return std::string(token);
}

inline std::string quote_header_param(std::string_view v) {
    std::string out;
    out.reserve(v.size() + 2);
    out.push_back('"');
    for (char c : v) {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        if (c != '\r' && c != '\n')
            out.push_back(c);
    }
    out.push_back('"');
    return out;
}

inline std::string build_www_authenticate(std::string_view resourceMetadataUrl,
                                          std::string_view error,
                                          std::span<const std::string> scopes,
                                          std::string_view errorDescription) {
    std::string h = "Bearer";
    bool first = true;
    auto addParam = [&](std::string_view k, std::string_view val) {
        h += first ? " " : ", ";
        first = false;
        h += k;
        h += "=";
        h += quote_header_param(val);
    };
    if (!resourceMetadataUrl.empty())
        addParam("resource_metadata", resourceMetadataUrl);
    if (!error.empty())
        addParam("error", error);
    if (!scopes.empty()) {
        std::string joined;
        for (size_t i = 0; i < scopes.size(); ++i) {
            if (i > 0)
                joined.push_back(' ');
            joined += scopes[i];
        }
        addParam("scope", joined);
    }
    if (!errorDescription.empty())
        addParam("error_description", errorDescription);
    return h;
}

inline TokenValidationResult evaluate_bearer_auth(const OAuthResourceConfig &oauth,
                                                  const HttpRouteRequest &req,
                                                  std::string_view expectedResource,
                                                  std::span<const std::string> requiredScopes) {
    auto authHdr = req.header("authorization");
    if (!authHdr) {
        return TokenValidationResult{
            .status = TokenValidationResult::Status::InvalidToken,
            .errorDescription = "Missing Authorization header",
            .requiredScopes = std::vector<std::string>(requiredScopes.begin(), requiredScopes.end()),
        };
    }
    auto token = extract_bearer_token(*authHdr);
    if (!token) {
        return TokenValidationResult{
            .status = TokenValidationResult::Status::InvalidToken,
            .errorDescription = "Invalid Authorization Bearer header",
            .requiredScopes = std::vector<std::string>(requiredScopes.begin(), requiredScopes.end()),
        };
    }
    if (!oauth.validateToken) {
        return TokenValidationResult{
            .status = TokenValidationResult::Status::ServiceUnavailable,
            .errorDescription = "Token validator is not configured",
        };
    }
    TokenValidationResult res = oauth.validateToken(TokenValidationContext{
        .token = *token,
        .expectedResource = expectedResource,
        .request = req,
    });
    if (res.status != TokenValidationResult::Status::Valid)
        return res;
    if (!res.principal.authenticated) {
        res.status = TokenValidationResult::Status::InvalidToken;
        if (res.errorDescription.empty())
            res.errorDescription = "Token did not yield an authenticated principal";
        return res;
    }
    if (!requiredScopes.empty() && !res.principal.hasScope("*")) {
        std::span<const std::string> reqSpan(requiredScopes.data(), requiredScopes.size());
        if (!res.principal.hasAllScopes(reqSpan)) {
            res.status = TokenValidationResult::Status::InsufficientScope;
            res.requiredScopes.assign(requiredScopes.begin(), requiredScopes.end());
            if (res.errorDescription.empty())
                res.errorDescription = "Token lacks required scopes";
            return res;
        }
    }
    return res;
}

inline bool checkAuthorization(const ServeApp &app,
                               const Principal &principal,
                               std::string_view operation,
                               std::span<const std::string> toolRequiredScopes,
                               std::string &outError) {
    if (!toolRequiredScopes.empty() && !principal.hasScope("*")) {
        if (!principal.hasAllScopes(toolRequiredScopes)) {
            outError = "Insufficient scope for operation: " + std::string(operation);
            return false;
        }
    }
    if (!app.defaultRequiredScopes.empty() && !principal.hasScope("*")) {
        std::span<const std::string> defSpan(app.defaultRequiredScopes.data(), app.defaultRequiredScopes.size());
        if (!principal.hasAllScopes(defSpan)) {
            outError = "Insufficient default scope for operation: " + std::string(operation);
            return false;
        }
    }
    if (app.authorizer) {
        return app.authorizer(principal, operation, outError);
    }
    return true;
}

} // namespace serve_detail
#endif // SLIMR_SERVE_HAS_AUTH

namespace serve_detail {

inline bool is_valid_ip_literal(std::string_view host, bool *isIpv6 = nullptr) {
    if (host.empty() || host.size() >= 64)
        return false;
    std::string tmp(host);
    struct in_addr v4{};
    if (inet_pton(AF_INET, tmp.c_str(), &v4) == 1) {
        if (isIpv6)
            *isIpv6 = false;
        return true;
    }
    struct in6_addr v6{};
    if (inet_pton(AF_INET6, tmp.c_str(), &v6) == 1) {
        if (isIpv6)
            *isIpv6 = true;
        return true;
    }
    return false;
}

inline std::string format_authority_host(std::string_view host, bool isIpv6) {
    if (isIpv6 && !host.empty() && host.front() != '[') {
        return "[" + std::string(host) + "]";
    }
    return std::string(host);
}

inline std::string strip_http_params(std::string_view v) {
    auto semi = v.find(';');
    if (semi != std::string_view::npos)
        v = v.substr(0, semi);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
        v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
        v.remove_suffix(1);
    return ascii_lower(v);
}

inline bool is_json_content_type(std::string_view headerVal) {
    return strip_http_params(headerVal) == "application/json";
}

inline bool accepts_ndjson(std::string_view acceptHeader) {
    size_t start = 0;
    while (start <= acceptHeader.size()) {
        auto comma = acceptHeader.find(',', start);
        std::string_view part = comma == std::string_view::npos
                                    ? acceptHeader.substr(start)
                                    : acceptHeader.substr(start, comma - start);
        if (strip_http_params(part) == "application/x-ndjson")
            return true;
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return false;
}

inline std::map<std::string, std::string> parse_query_params(std::string_view rawUrl) {
    std::map<std::string, std::string> out;
    auto qpos = rawUrl.find('?');
    if (qpos == std::string_view::npos)
        return out;
    std::string_view qs = rawUrl.substr(qpos + 1);
    auto hash = qs.find('#');
    if (hash != std::string_view::npos)
        qs = qs.substr(0, hash);

    auto urlDecode = [](std::string_view s) {
        std::string res;
        res.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '+') {
                res.push_back(' ');
            } else if (s[i] == '%' && i + 2 < s.size()) {
                auto hexVal = [](char c) -> int {
                    if (c >= '0' && c <= '9')
                        return c - '0';
                    if (c >= 'a' && c <= 'f')
                        return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F')
                        return c - 'A' + 10;
                    return -1;
                };
                int hi = hexVal(s[i + 1]);
                int lo = hexVal(s[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    res.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
                res.push_back(s[i]);
            } else {
                res.push_back(s[i]);
            }
        }
        return res;
    };

    size_t pos = 0;
    while (pos <= qs.size()) {
        auto amp = qs.find('&', pos);
        std::string_view pair = amp == std::string_view::npos ? qs.substr(pos) : qs.substr(pos, amp - pos);
        if (!pair.empty()) {
            auto eq = pair.find('=');
            if (eq == std::string_view::npos) {
                out[urlDecode(pair)] = "";
            } else {
                out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            }
        }
        if (amp == std::string_view::npos)
            break;
        pos = amp + 1;
    }
    return out;
}

} // namespace serve_detail

/** Configuration options for ServeHttpServer. */
struct ServeHttpOptions {
    std::string host{"0.0.0.0"};
    int port{0};
    std::string publicBaseUrl;
    size_t maxRequestBytes{size_t{4} * 1024 * 1024};
    uint32_t maxConnections{64};
    size_t workerThreads{8};
    int shutdownTimeoutMs{5000};
    int keepaliveTimeoutMs{30000};
    OAuthResourceConfig oauth;
    std::vector<CustomHttpRoute> customRoutes;
};

using HttpServerOptions = ServeHttpOptions;

/** Plain JSON + NDJSON HTTP server for ServeApp using cpp-httplib and D13 graceful drain. */
class ServeHttpServer {
  public:
    explicit ServeHttpServer(ServeApp &app, ServeHttpOptions opts = {})
        : app_(app), opts_(std::move(opts)), drain_(std::make_shared<DrainState>()) {}

    ServeHttpServer(const ServeHttpServer &) = delete;
    ServeHttpServer &operator=(const ServeHttpServer &) = delete;

    ~ServeHttpServer() { stop(); }

    uint16_t boundPort() const noexcept { return boundPort_.load(std::memory_order_acquire); }

    std::string baseUrl() const {
        if (!opts_.publicBaseUrl.empty()) {
            std::string b = opts_.publicBaseUrl;
            while (b.size() > 1 && b.back() == '/')
                b.pop_back();
            return b;
        }
        bool isV6 = false;
        serve_detail::is_valid_ip_literal(opts_.host, &isV6);
        return "http://" + serve_detail::format_authority_host(opts_.host, isV6) + ":" + std::to_string(boundPort());
    }

    std::string resourceMetadataUrl() const {
        std::string p = opts_.oauth.metadataPath.empty()
                            ? "/.well-known/oauth-protected-resource"
                            : opts_.oauth.metadataPath;
        return baseUrl() + p;
    }

    bool start(std::string *errOut = nullptr) {
        if (running_.load(std::memory_order_acquire))
            return true;

        bool isV6 = false;
        if (!serve_detail::is_valid_ip_literal(opts_.host, &isV6)) {
            if (errOut)
                *errOut = "Invalid bind host IP literal: " + opts_.host;
            return false;
        }
        if (opts_.port < 0 || opts_.port > 65535) {
            if (errOut)
                *errOut = "Invalid port: " + std::to_string(opts_.port);
            return false;
        }

        svr_ = std::make_unique<httplib::Server>();
        const size_t baseThreads = std::max<size_t>(8, opts_.workerThreads);
        const size_t maxThreads = std::max<size_t>(baseThreads, static_cast<size_t>(opts_.maxConnections) + 16);
        svr_->new_task_queue = [baseThreads, maxThreads]() {
            return new httplib::ThreadPool(baseThreads, maxThreads, 0);
        };
        svr_->set_payload_max_length(opts_.maxRequestBytes);
        svr_->set_keep_alive_timeout(std::max<time_t>(1, (opts_.keepaliveTimeoutMs + 999) / 1000));
        svr_->set_write_timeout(1, 0);

        // Reject draining requests and oversized Content-Length before reading bodies.
        svr_->set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
            if (draining_.load(std::memory_order_acquire)) {
                res.set_header("Connection", "close");
                res.status = 503;
                res.set_content("{\"error\":\"server_stopping\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.has_header("Content-Length")) {
                std::string cl = req.get_header_value("Content-Length");
                char *endPtr = nullptr;
                unsigned long long len = std::strtoull(cl.c_str(), &endPtr, 10);
                if (endPtr == cl.c_str() || *endPtr != '\0' || len > opts_.maxRequestBytes) {
                    res.status = 413;
                    res.set_content("{\"error\":\"payload_too_large\"}", "application/json");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        // Register custom routes BEFORE generic /:tool (httplib uses first-match-wins).
        for (const auto &route : opts_.customRoutes) {
            registerCustomRoute(route);
        }

        auto methodNotAllowedGet = [](const httplib::Request &, httplib::Response &res) {
            sendResponse(res, 405, "application/json", "{\"error\":\"method_not_allowed\"}",
                         {{"Allow", "GET"}});
        };
        svr_->Get("/health", [this](const httplib::Request &, httplib::Response &res) {
            Json body = json_obj();
            body["status"] = "ok";
            body["key"] = app_.key;
            body["version"] = app_.version;
            body["apiVersion"] = app_.apiVersion;
            sendJsonText(res, 200, slimr::write_json(body));
        });
        svr_->Post("/health", methodNotAllowedGet);
        svr_->Put("/health", methodNotAllowedGet);
        svr_->Delete("/health", methodNotAllowedGet);
        svr_->Patch("/health", methodNotAllowedGet);

        svr_->Get("/schema", [this](const httplib::Request &, httplib::Response &res) {
            Json toolsObj = json_obj();
            for (const auto &[tName, def] : app_.tools()) {
                Json entry = json_obj();
                entry["name"] = def.name;
                entry["description"] = def.description;
                entry["inputSchema"] = def.inputSchema;
                if (def.outputSchema.has_value())
                    entry["outputSchema"] = *def.outputSchema;
                Json ann = def.annotations.toJson();
                if (!ann.empty())
                    entry["annotations"] = std::move(ann);
                entry["runsLocal"] = def.runsLocal;
                entry["threadSafe"] = def.threadSafe;
                toolsObj[tName] = std::move(entry);
            }
            Json body = json_obj();
            body["name"] = app_.key;
            body["version"] = app_.version;
            body["apiVersion"] = app_.apiVersion;
            body["tools"] = std::move(toolsObj);
            sendJsonText(res, 200, slimr::write_json(body));
        });
        svr_->Post("/schema", methodNotAllowedGet);
        svr_->Put("/schema", methodNotAllowedGet);
        svr_->Delete("/schema", methodNotAllowedGet);
        svr_->Patch("/schema", methodNotAllowedGet);

        svr_->Post("/:tool", [this](const httplib::Request &req, httplib::Response &res) {
            try {
                handleToolPost(req, res);
            } catch (const std::exception &e) {
                Json err = json_obj();
                err["error"] = "internal_error";
                err["message"] = redact_sensitive(e.what());
                sendJsonText(res, 500, slimr::write_json(err));
            } catch (...) {
                sendJsonText(res, 500, "{\"error\":\"internal_error\"}");
            }
        });
        auto methodNotAllowedPost = [](const httplib::Request &, httplib::Response &res) {
            sendResponse(res, 405, "application/json", "{\"error\":\"method_not_allowed\"}",
                         {{"Allow", "POST"}});
        };
        svr_->Get("/:tool", methodNotAllowedPost);
        svr_->Put("/:tool", methodNotAllowedPost);
        svr_->Delete("/:tool", methodNotAllowedPost);
        svr_->Patch("/:tool", methodNotAllowedPost);

        draining_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(drain_->mu);
            drain_->activeCount = 0;
            drain_->inFlightResponses = 0;
            drain_->activeCancels.clear();
        }

        int actualPort = -1;
        if (opts_.port == 0) {
            actualPort = svr_->bind_to_any_port(opts_.host);
        } else if (svr_->bind_to_port(opts_.host, opts_.port)) {
            actualPort = opts_.port;
        }
        if (actualPort <= 0) {
            svr_.reset();
            if (errOut)
                *errOut = "Failed to bind HTTP server on " + opts_.host + ":" + std::to_string(opts_.port);
            return false;
        }

        acceptThread_ = std::thread([this]() { svr_->listen_after_bind(); });
        svr_->wait_until_ready();
        if (!svr_->is_running()) {
            svr_->stop();
            if (acceptThread_.joinable())
                acceptThread_.join();
            svr_.reset();
            if (errOut)
                *errOut = "Failed to start HTTP server on " + opts_.host + ":" + std::to_string(actualPort);
            return false;
        }

        boundPort_.store(static_cast<uint16_t>(actualPort), std::memory_order_release);
        running_.store(true, std::memory_order_release);
        return true;
    }

    void stop() {
        bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
        if (!wasRunning)
            return;

        draining_.store(true, std::memory_order_release);

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(std::max(0, opts_.shutdownTimeoutMs));
        bool drainedNaturally = false;
        {
            std::unique_lock lock(drain_->mu);
            drainedNaturally = drain_->idleCv.wait_until(lock, deadline, [&]() {
                return drain_->activeCount == 0;
            });
            for (auto &wk : drain_->activeCancels) {
                if (auto src = wk.lock())
                    src->cancel();
            }
        }

        if (drainedNaturally) {
            std::unique_lock lock(drain_->mu);
            drain_->responsesCv.wait_until(lock, deadline, [&]() {
                return drain_->inFlightResponses == 0;
            });
        }

        if (svr_)
            svr_->stop();
        if (acceptThread_.joinable())
            acceptThread_.join();
        svr_.reset();
        boundPort_.store(0, std::memory_order_release);
    }

    int runUntilSignal() {
        static std::atomic<bool> *sStopFlag = nullptr;
        std::atomic<bool> stopRequested{false};
        sStopFlag = &stopRequested;

        auto sigHandler = [](int) {
            if (sStopFlag)
                sStopFlag->store(true, std::memory_order_release);
        };
        auto prevInt = std::signal(SIGINT, sigHandler);
        auto prevTerm = std::signal(SIGTERM, sigHandler);

        while (running_.load(std::memory_order_acquire) &&
               !stopRequested.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::signal(SIGINT, prevInt);
        std::signal(SIGTERM, prevTerm);
        sStopFlag = nullptr;
        stop();
        return 0;
    }

  private:
    struct DrainState {
        std::mutex mu;
        std::condition_variable idleCv;
        std::condition_variable responsesCv;
        std::vector<std::weak_ptr<CancellationSource>> activeCancels;
        size_t activeCount{0};
        size_t inFlightResponses{0};
    };

    struct NdjsonStreamState {
        std::mutex mu;
        std::condition_variable cv;
        std::deque<std::string> lines;
        bool finished{false};
    };

    static void sendResponse(httplib::Response &res,
                             int status,
                             std::string_view contentType,
                             const std::string &body,
                             std::vector<std::pair<std::string, std::string>> extraHeaders = {}) {
        res.status = status;
        for (const auto &[k, v] : extraHeaders) {
            res.set_header(k, v);
        }
        res.set_content(body, std::string(contentType));
    }

    static void sendJsonText(httplib::Response &res,
                             int status,
                             const std::string &jsonStr,
                             std::vector<std::pair<std::string, std::string>> extraHeaders = {}) {
        sendResponse(res, status, "application/json", jsonStr, std::move(extraHeaders));
    }

    static HttpRouteRequest toRouteRequest(const httplib::Request &req) {
        HttpRouteRequest out;
        out.method = req.method;
        out.path = req.path;
        out.rawQuery = req.target;
        out.queryParams = serve_detail::parse_query_params(req.target);
        for (const auto &[k, v] : req.headers) {
            out.headers[serve_detail::ascii_lower(k)] = v;
        }
        out.body = req.body;
        out.remoteAddr = req.remote_addr + ":" + std::to_string(req.remote_port);
        return out;
    }

    void registerCustomRoute(const CustomHttpRoute &route) {
        if (!route.handler || route.path.empty())
            return;
        auto routePtr = std::make_shared<CustomHttpRoute>(route);
        auto wrap = [this, routePtr](const httplib::Request &req, httplib::Response &res) {
            try {
                if (req.body.size() > opts_.maxRequestBytes) {
                    sendJsonText(res, 413, "{\"error\":\"payload_too_large\"}");
                    return;
                }
                HttpRouteRequest rreq = toRouteRequest(req);
                HttpRouteResponse rresp = routePtr->handler(rreq);
                std::vector<std::pair<std::string, std::string>> hdrs;
                hdrs.reserve(rresp.headers.size());
                for (const auto &[k, v] : rresp.headers) {
                    hdrs.emplace_back(k, v);
                }
                sendResponse(res, rresp.status, rresp.contentType, rresp.body, std::move(hdrs));
            } catch (const std::exception &e) {
                Json err = json_obj();
                err["error"] = "internal_error";
                err["message"] = redact_sensitive(e.what());
                sendJsonText(res, 500, slimr::write_json(err));
            } catch (...) {
                sendJsonText(res, 500, "{\"error\":\"internal_error\"}");
            }
        };

        std::string m = serve_detail::ascii_lower(route.method);
        if (m == "get")
            svr_->Get(route.path, wrap);
        else if (m == "post")
            svr_->Post(route.path, wrap);
        else if (m == "delete")
            svr_->Delete(route.path, wrap);
        else if (m == "put")
            svr_->Put(route.path, wrap);
        else if (m == "patch")
            svr_->Patch(route.path, wrap);
        else if (m == "options")
            svr_->Options(route.path, wrap);
        else if (m == "*") {
            svr_->Get(route.path, wrap);
            svr_->Post(route.path, wrap);
            svr_->Delete(route.path, wrap);
            svr_->Put(route.path, wrap);
            svr_->Patch(route.path, wrap);
            svr_->Options(route.path, wrap);
        }
    }

    void sendOAuthUnauthorized(httplib::Response &res,
                               std::span<const std::string> scopes,
                               std::string_view desc) const {
        std::string www = serve_detail::build_www_authenticate(
            resourceMetadataUrl(), "", scopes, "");
        Json body = json_obj();
        body["error"] = "unauthorized";
        body["error_description"] = std::string(desc);
        sendJsonText(res, 401, slimr::write_json(body), {{"WWW-Authenticate", std::move(www)}});
    }

    void sendOAuthInsufficientScope(httplib::Response &res,
                                    std::span<const std::string> scopes,
                                    std::string_view desc) const {
        std::string www = serve_detail::build_www_authenticate(
            resourceMetadataUrl(), "insufficient_scope", scopes, desc);
        Json body = json_obj();
        body["error"] = "insufficient_scope";
        body["error_description"] = std::string(desc);
        sendJsonText(res, 403, slimr::write_json(body), {{"WWW-Authenticate", std::move(www)}});
    }

    void handleToolPost(const httplib::Request &req, httplib::Response &res) {
        if (req.body.size() > opts_.maxRequestBytes) {
            sendJsonText(res, 413, "{\"error\":\"payload_too_large\"}");
            return;
        }

        if (req.has_header("X-Slimr-Api-Version")) {
            std::string verStr = req.get_header_value("X-Slimr-Api-Version");
            char *endPtr = nullptr;
            long clientVer = std::strtol(verStr.c_str(), &endPtr, 10);
            if (endPtr == verStr.c_str() || *endPtr != '\0' || clientVer < 0) {
                sendJsonText(res, 400, "{\"error\":\"invalid_api_version\",\"message\":\"Invalid X-Slimr-Api-Version header\"}");
                return;
            }
            if (clientVer > app_.apiVersion) {
                Json body = json_obj();
                body["error"] = "upgrade_required";
                body["message"] = "daemon older than client; restart it";
                body["serverApiVersion"] = app_.apiVersion;
                body["clientApiVersion"] = clientVer;
                sendJsonText(res, 426, slimr::write_json(body));
                return;
            }
        }

        if (req.has_header("Content-Type") &&
            !serve_detail::is_json_content_type(req.get_header_value("Content-Type"))) {
            sendJsonText(res, 415, "{\"error\":\"unsupported_media_type\",\"message\":\"Content-Type must be application/json\"}");
            return;
        }
        if (!req.has_header("Content-Type") && !req.body.empty()) {
            sendJsonText(res, 415, "{\"error\":\"unsupported_media_type\",\"message\":\"Content-Type must be application/json\"}");
            return;
        }

        auto toolIt = req.path_params.find("tool");
        std::string toolName = toolIt != req.path_params.end() ? toolIt->second : "";
        const ToolDef *def = app_.findTool(toolName);
        if (!def) {
            Json body = json_obj();
            body["error"] = "not_found";
            body["message"] = "Tool not found: " + toolName;
            sendJsonText(res, 404, slimr::write_json(body));
            return;
        }

        HttpRouteRequest routeReq = toRouteRequest(req);
        Principal principal;
        if (opts_.oauth.enabled) {
            std::vector<std::string> reqScopes = def->requiredScopes.empty()
                                                     ? opts_.oauth.defaultScopes
                                                     : def->requiredScopes;
            std::span<const std::string> scopeSpan(reqScopes.data(), reqScopes.size());
            auto authRes = serve_detail::evaluate_bearer_auth(opts_.oauth, routeReq, baseUrl(), scopeSpan);
            if (authRes.status == TokenValidationResult::Status::InvalidToken) {
                sendOAuthUnauthorized(res, authRes.requiredScopes, authRes.errorDescription);
                return;
            }
            if (authRes.status == TokenValidationResult::Status::InsufficientScope) {
                sendOAuthInsufficientScope(res, authRes.requiredScopes, authRes.errorDescription);
                return;
            }
            if (authRes.status == TokenValidationResult::Status::ServiceUnavailable) {
                sendJsonText(res, 503, "{\"error\":\"service_unavailable\",\"message\":\"Auth service unavailable\"}");
                return;
            }
            principal = std::move(authRes.principal);
        }

        std::string authErr;
        std::span<const std::string> toolScopeSpan(def->requiredScopes.data(), def->requiredScopes.size());
        if (!serve_detail::checkAuthorization(app_, principal, def->name, toolScopeSpan, authErr)) {
            Json body = json_obj();
            body["error"] = "forbidden";
            body["message"] = authErr;
            sendJsonText(res, 403, slimr::write_json(body));
            return;
        }

        Json parsedBody = json_obj();
        if (!req.body.empty()) {
            auto parsed = parse_json<Json>(req.body);
            if (!parsed) {
                sendJsonText(res, 400, "{\"error\":\"invalid_json\",\"message\":\"Malformed JSON request body\"}");
                return;
            }
            if (!parsed->is_object()) {
                sendJsonText(res, 400, "{\"error\":\"invalid_params\",\"message\":\"Request body must be a JSON object\"}");
                return;
            }
            parsedBody = std::move(*parsed);
        }

        Json args = parsedBody;
        RequestContext ctx;
        ctx.principal = std::move(principal);
        bool hasInputSchemaProp = def->inputSchema.contains("properties") &&
                                  def->inputSchema["properties"].is_object() &&
                                  def->inputSchema["properties"].contains("input");
        bool isEnvelope = parsedBody.contains("input") &&
                          (parsedBody.contains("context") || (parsedBody.size() == 1 && !hasInputSchemaProp));
        if (isEnvelope) {
            if (!parsedBody["input"].is_object()) {
                sendJsonText(res, 400, "{\"error\":\"invalid_params\",\"message\":\"Envelope 'input' must be a JSON object\"}");
                return;
            }
            args = parsedBody["input"];
            if (parsedBody.contains("context")) {
                if (!parsedBody["context"].is_object()) {
                    sendJsonText(res, 400, "{\"error\":\"invalid_params\",\"message\":\"Envelope 'context' must be a JSON object\"}");
                    return;
                }
                const Json &c = parsedBody["context"];
                if (c.contains("session") && c["session"].is_string()) {
                    ctx.session = c["session"].get<std::string>();
                }
            }
        }

        bool wantNdjson = req.has_header("Accept") &&
                          serve_detail::accepts_ndjson(req.get_header_value("Accept"));

        auto cancelSrc = std::make_shared<CancellationSource>();
        ctx.cancellation = cancelSrc->token();

        {
            std::lock_guard lock(drain_->mu);
            if (draining_.load(std::memory_order_acquire)) {
                res.set_header("Connection", "close");
                sendJsonText(res, 503, "{\"error\":\"server_stopping\"}");
                return;
            }
            ++drain_->activeCount;
            ++drain_->inFlightResponses;
            drain_->activeCancels.push_back(cancelSrc);
        }

        auto finishActive = [drain = drain_, cancelSrc]() {
            std::lock_guard lock(drain->mu);
            if (drain->activeCount > 0)
                --drain->activeCount;
            std::erase_if(drain->activeCancels, [&cancelSrc](const auto &wk) {
                auto locked = wk.lock();
                return !locked || locked == cancelSrc;
            });
            if (drain->activeCount == 0)
                drain->idleCv.notify_all();
        };

        auto finishResponse = [drain = drain_]() {
            std::lock_guard lock(drain->mu);
            if (drain->inFlightResponses > 0)
                --drain->inFlightResponses;
            if (drain->inFlightResponses == 0)
                drain->responsesCv.notify_all();
        };

        if (!wantNdjson) {
            ServeResult toolRes;
            try {
                if (app_.concurrency == ConcurrencyPolicy::Serialized && !def->threadSafe) {
                    std::lock_guard execLock(app_.execMutex());
                    toolRes = def->handler(args, ctx);
                } else {
                    toolRes = def->handler(args, ctx);
                }
            } catch (const ServeException &e) {
                finishActive();
                Json body = json_obj();
                body["error"] = "tool_error";
                body["code"] = e.code;
                body["message"] = redact_sensitive(e.what());
                sendJsonText(res, e.httpStatus, slimr::write_json(body));
                finishResponse();
                return;
            } catch (const std::exception &e) {
                finishActive();
                Json body = json_obj();
                body["error"] = "internal_error";
                body["message"] = redact_sensitive(e.what());
                sendJsonText(res, 500, slimr::write_json(body));
                finishResponse();
                return;
            }

            finishActive();
            if (toolRes.isError) {
                int status = toolRes.httpStatus >= 400 ? toolRes.httpStatus : 400;
                Json body = json_obj();
                body["error"] = "tool_error";
                body["message"] = redact_sensitive(toolRes.errorMessage);
                sendJsonText(res, status, slimr::write_json(body));
            } else {
                sendJsonText(res, 200, slimr::write_json(toolRes.toJson()));
            }
            finishResponse();
            return;
        }

        // Streaming NDJSON path: run tool on a worker thread while chunked provider drains lines.
        auto stream = std::make_shared<NdjsonStreamState>();
        auto channel = std::make_shared<NotificationChannel>([stream](const Json &ev) {
            std::lock_guard lock(stream->mu);
            stream->lines.push_back(slimr::write_json(ev) + "\n");
            stream->cv.notify_one();
        });
        ctx.notificationChannel = channel;

        auto pushLine = [stream](const Json &obj) {
            std::lock_guard lock(stream->mu);
            stream->lines.push_back(slimr::write_json(obj) + "\n");
            stream->finished = true;
            stream->cv.notify_all();
        };

        std::thread([&app = app_, def, args = std::move(args), ctx, channel, cancelSrc, stream, pushLine, finishActive]() mutable {
            try {
                ServeResult toolRes;
                if (app.concurrency == ConcurrencyPolicy::Serialized && !def->threadSafe) {
                    std::lock_guard execLock(app.execMutex());
                    toolRes = def->handler(args, ctx);
                } else {
                    toolRes = def->handler(args, ctx);
                }
                channel->close();
                finishActive();
                if (toolRes.isError) {
                    Json errEv = json_obj();
                    errEv["type"] = "error";
                    errEv["status"] = toolRes.httpStatus >= 400 ? toolRes.httpStatus : 400;
                    errEv["message"] = redact_sensitive(toolRes.errorMessage);
                    pushLine(errEv);
                } else {
                    Json resEv = json_obj();
                    resEv["type"] = "result";
                    resEv["data"] = toolRes.toJson();
                    pushLine(resEv);
                }
            } catch (const ServeException &e) {
                channel->close();
                finishActive();
                Json errEv = json_obj();
                errEv["type"] = "error";
                errEv["status"] = e.httpStatus;
                errEv["code"] = e.code;
                errEv["message"] = redact_sensitive(e.what());
                pushLine(errEv);
            } catch (const std::exception &e) {
                channel->close();
                finishActive();
                Json errEv = json_obj();
                errEv["type"] = "error";
                errEv["status"] = 500;
                errEv["message"] = redact_sensitive(e.what());
                pushLine(errEv);
            } catch (...) {
                channel->close();
                finishActive();
                Json errEv = json_obj();
                errEv["type"] = "error";
                errEv["status"] = 500;
                errEv["message"] = "Unknown error";
                pushLine(errEv);
            }
        }).detach();

        res.status = 200;
        res.set_chunked_content_provider(
            "application/x-ndjson",
            [stream, cancelSrc](size_t, httplib::DataSink &sink) {
                for (;;) {
                    std::string line;
                    bool isDone = false;
                    {
                        std::unique_lock lock(stream->mu);
                        stream->cv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
                            return !stream->lines.empty() || stream->finished;
                        });
                        if (!stream->lines.empty()) {
                            line = std::move(stream->lines.front());
                            stream->lines.pop_front();
                        } else if (stream->finished) {
                            isDone = true;
                        }
                    }
                    if (!line.empty()) {
                        if (!sink.write(line.data(), line.size())) {
                            cancelSrc->cancel();
                            return false;
                        }
                        return true;
                    }
                    if (isDone) {
                        sink.done();
                        return true;
                    }
                    if (!sink.is_writable()) {
                        cancelSrc->cancel();
                        return false;
                    }
                }
            },
            [finishResponse](bool) {
                finishResponse();
            });
    }

    ServeApp &app_;
    ServeHttpOptions opts_;
    std::shared_ptr<DrainState> drain_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread acceptThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> draining_{false};
    std::atomic<uint16_t> boundPort_{0};
};

/** Attaches standalone `serve [--host <addr>] [--port <n>]` (and `http` alias) subcommand to ServeApp. */
inline void enable_serve(ServeApp &app, ServeHttpOptions defaults = {}) {
    app.setHttpRunner([defaults = std::move(defaults)](ServeApp &target, int argc, char **argv) -> int {
        ServeHttpOptions opts = defaults;
        std::string subcmd = argc >= 2 ? std::string(argv[1]) : "serve";
        for (int i = 2; i < argc; ++i) {
            std::string_view arg = argv[i];
            if (arg == "--host" && i + 1 < argc) {
                opts.host = argv[++i];
            } else if (arg == "--port" && i + 1 < argc) {
                std::string portStr = argv[++i];
                char *endPtr = nullptr;
                long parsedPort = std::strtol(portStr.c_str(), &endPtr, 10);
                if (endPtr == portStr.c_str() || *endPtr != '\0' || parsedPort < 0 || parsedPort > 65535) {
                    std::cerr << "Invalid --port value: " << portStr << "\n";
                    return 2;
                }
                opts.port = static_cast<int>(parsedPort);
            } else {
                std::cerr << "Unknown or incomplete option: " << arg << "\n";
                std::cerr << "Usage: " << (target.key.empty() ? "app" : target.key)
                          << " " << subcmd << " [--host <addr>] [--port <n>]\n";
                return 2;
            }
        }

        ServeHttpServer server(target, std::move(opts));
        std::string errMsg;
        if (!server.start(&errMsg)) {
            std::cerr << "HTTP server failed to start: " << errMsg << "\n";
            return 1;
        }
        std::cerr << (target.key.empty() ? "app" : target.key)
                  << " listening on " << server.baseUrl() << "\n"
                  << std::flush;
        return server.runUntilSignal();
    });
}

/** Alias for enable_serve() preserving existing plugin call sites. */
inline void enable_http(ServeApp &app, ServeHttpOptions defaults = {}) {
    enable_serve(app, std::move(defaults));
}

} // namespace slimr
