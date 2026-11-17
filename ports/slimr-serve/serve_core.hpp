#pragma once
#include "bugsink.hpp"
#include "json.hpp"
#include "reflect.hpp"

#include <algorithm>
#include <any>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace slimr {

namespace ServeErrorCode {
inline constexpr int InvalidParams = -32602;
inline constexpr int InternalError = -32603;
inline constexpr int RequestCancelled = -32000;
inline constexpr int InsufficientScope = -32001;
} // namespace ServeErrorCode

namespace serve_detail {

inline std::string ascii_lower(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (unsigned char c : sv) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

inline bool is_sensitive_key(std::string_view key) {
    std::string lower = ascii_lower(key);
    return lower == "authorization" || lower == "proxy-authorization" ||
           lower == "cookie" || lower == "set-cookie" ||
           lower == "access_token" || lower == "refresh_token" ||
           lower == "id_token" || lower == "client_secret" ||
           lower == "token" || lower == "secret" ||
           lower == "password" || lower == "api_key" ||
           lower == "apikey" || lower == "code" ||
           lower == "code_verifier" || lower == "x-api-key";
}

inline void redact_param_in_place(std::string &text, std::string_view param) {
    if (text.empty() || param.empty())
        return;
    std::string lower = ascii_lower(text);
    std::string needle = ascii_lower(param) + "=";
    size_t pos = 0;
    while ((pos = lower.find(needle, pos)) != std::string::npos) {
        if (pos > 0) {
            char prev = lower[pos - 1];
            if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_') {
                pos += needle.size();
                continue;
            }
        }
        size_t valStart = pos + needle.size();
        size_t valEnd = valStart;
        while (valEnd < text.size() && text[valEnd] != '&' &&
               text[valEnd] != ' ' && text[valEnd] != '"' &&
               text[valEnd] != '\'' && text[valEnd] != '\n' &&
               text[valEnd] != '\r') {
            ++valEnd;
        }
        text.replace(valStart, valEnd - valStart, "[REDACTED]");
        lower = ascii_lower(text);
        pos = valStart + 10;
    }
}

inline void redact_scheme_in_place(std::string &text, std::string_view scheme) {
    if (text.empty())
        return;
    std::string lower = ascii_lower(text);
    std::string needle = ascii_lower(scheme) + " ";
    size_t pos = 0;
    while ((pos = lower.find(needle, pos)) != std::string::npos) {
        if (pos > 0 && std::isalnum(static_cast<unsigned char>(lower[pos - 1]))) {
            pos += needle.size();
            continue;
        }
        size_t valStart = pos + needle.size();
        while (valStart < text.size() && text[valStart] == ' ')
            ++valStart;
        size_t valEnd = valStart;
        while (valEnd < text.size() && text[valEnd] != ' ' &&
               text[valEnd] != '"' && text[valEnd] != '\'' &&
               text[valEnd] != '\n' && text[valEnd] != '\r' &&
               text[valEnd] != ',' && text[valEnd] != ';') {
            ++valEnd;
        }
        if (valEnd > valStart) {
            text.replace(valStart, valEnd - valStart, "[REDACTED]");
            lower = ascii_lower(text);
            pos = valStart + 10;
        } else {
            pos = valStart;
        }
    }
}

inline int log_level_severity(std::string_view level) noexcept {
    if (level == "debug")
        return 0;
    if (level == "info")
        return 1;
    if (level == "notice")
        return 2;
    if (level == "warning")
        return 3;
    if (level == "error")
        return 4;
    if (level == "critical")
        return 5;
    if (level == "alert")
        return 6;
    if (level == "emergency")
        return 7;
    return -1;
}

} // namespace serve_detail

/** Redacts bearer tokens, authorization headers, and secret URL query parameters from text. */
inline std::string redact_sensitive(std::string_view input) {
    std::string out(input);
    serve_detail::redact_scheme_in_place(out, "Bearer");
    serve_detail::redact_scheme_in_place(out, "Basic");
    static constexpr std::string_view kSensitiveParams[] = {
        "access_token", "refresh_token", "id_token", "client_secret",
        "code_verifier", "code", "token", "secret", "password",
        "api_key", "apikey", "key"};
    for (std::string_view p : kSensitiveParams) {
        serve_detail::redact_param_in_place(out, p);
    }
    return out;
}

/** Recursively redacts sensitive keys and secret-bearing string values inside a JSON value. */
inline Json redact_json(const Json &val) {
    if (val.is_object()) {
        Json out = json_obj();
        for (const auto &[k, v] : val.get_object()) {
            if (serve_detail::is_sensitive_key(k)) {
                out[k] = "[REDACTED]";
            } else {
                out[k] = redact_json(v);
            }
        }
        return out;
    }
    if (val.is_array()) {
        Json::array_t arr;
        arr.reserve(val.get_array().size());
        for (const auto &el : val.get_array()) {
            arr.push_back(redact_json(el));
        }
        return json_arr(std::move(arr));
    }
    if (val.is_string()) {
        return Json(redact_sensitive(val.get<std::string>()));
    }
    return val;
}

/** Authenticated caller identity and authorization grants for a single request. */
struct Principal {
    bool authenticated{false};
    std::string subject;
    std::string clientId;
    std::string issuer;
    std::string audience;
    std::vector<std::string> scopes;
    std::optional<std::chrono::system_clock::time_point> expiresAt;
    Json claims = json_obj();

    bool hasScope(std::string_view scope) const noexcept {
        for (const auto &s : scopes) {
            if (s == scope)
                return true;
        }
        return false;
    }

    bool hasAllScopes(std::span<const std::string_view> required) const noexcept {
        for (std::string_view r : required) {
            if (!hasScope(r))
                return false;
        }
        return true;
    }

    bool hasAllScopes(std::span<const std::string> required) const noexcept {
        for (const auto &r : required) {
            if (!hasScope(r))
                return false;
        }
        return true;
    }
};

/** Thread-safe cooperative cancellation token observed by request handlers and sinks. */
class CancellationToken {
  public:
    struct State {
        std::atomic<bool> cancelled{false};
        std::mutex mu;
        uint64_t nextId{1};
        std::vector<std::pair<uint64_t, std::function<void()>>> callbacks;
    };

    CancellationToken() = default;
    explicit CancellationToken(std::shared_ptr<State> state) noexcept
        : state_(std::move(state)) {}

    bool canBeCancelled() const noexcept { return state_ != nullptr; }

    bool isCancelled() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }

    uint64_t onCancel(std::function<void()> cb) const {
        if (!state_ || !cb)
            return 0;
        std::unique_lock lock(state_->mu);
        if (state_->cancelled.load(std::memory_order_acquire)) {
            lock.unlock();
            try {
                cb();
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
            return 0;
        }
        uint64_t id = state_->nextId++;
        state_->callbacks.emplace_back(id, std::move(cb));
        return id;
    }

    void removeOnCancel(uint64_t id) const {
        if (!state_ || id == 0)
            return;
        std::lock_guard lock(state_->mu);
        std::erase_if(state_->callbacks, [id](const auto &entry) { return entry.first == id; });
    }

  private:
    std::shared_ptr<State> state_;
};

/** Owns the trigger side of a CancellationToken. */
class CancellationSource {
  public:
    CancellationSource() : state_(std::make_shared<CancellationToken::State>()) {}

    CancellationToken token() const noexcept { return CancellationToken(state_); }

    bool isCancelled() const noexcept {
        return state_->cancelled.load(std::memory_order_acquire);
    }

    void cancel() const noexcept {
        bool expected = false;
        if (!state_->cancelled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        std::vector<std::pair<uint64_t, std::function<void()>>> cbs;
        {
            std::lock_guard lock(state_->mu);
            cbs.swap(state_->callbacks);
        }
        for (auto &[id, fn] : cbs) {
            (void)id;
            try {
                if (fn)
                    fn();
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
        }
    }

  private:
    std::shared_ptr<CancellationToken::State> state_;
};

/** Protocol-agnostic request exception carrying error code, HTTP status, and optional data. */
class ServeException : public std::runtime_error {
  public:
    int code{ServeErrorCode::InternalError};
    int httpStatus{500};
    Json data = Json{};

    ServeException(int errCode, std::string message, Json errData = Json{}, int status = 400)
        : std::runtime_error(std::move(message)), code(errCode), httpStatus(status), data(std::move(errData)) {}
};

/** Thread-safe request-scoped notification channel bound by the active transport. */
class NotificationChannel {
  public:
    using SinkFn = std::function<void(const Json &)>;

    explicit NotificationChannel(SinkFn sink = nullptr) : sink_(std::move(sink)) {}

    void emit(const Json &event) {
        SinkFn fn;
        {
            std::lock_guard lock(mu_);
            if (closed_ || !sink_)
                return;
            fn = sink_;
        }
        fn(event);
    }

    void close() noexcept {
        std::lock_guard lock(mu_);
        closed_ = true;
        sink_ = nullptr;
    }

  private:
    std::mutex mu_;
    bool closed_{false};
    SinkFn sink_;
};

/** Per-request metadata, caller principal, cancellation token, and notification emitters. */
struct RequestContext {
    std::string protocolVersion;
    Json clientCapabilities = json_obj();
    Json clientInfo = json_obj();
    Json requestMeta = json_obj();
    std::optional<Json> requestId;
    std::optional<Json> progressToken;
    std::optional<std::string> minLogLevel;
    std::optional<Json> inputResponses;
    std::optional<std::string> requestState;
    std::optional<std::string> session;
    Principal principal;
    CancellationToken cancellation;
    std::shared_ptr<NotificationChannel> notificationChannel;

    void emitProgress(double progress,
                      std::optional<double> total = std::nullopt,
                      std::optional<std::string> message = std::nullopt) const {
        if (!notificationChannel || cancellation.isCancelled()) {
            return;
        }
        Json ev = json_obj();
        ev["type"] = "progress";
        ev["progress"] = progress;
        if (total.has_value()) {
            ev["total"] = *total;
        }
        if (message.has_value()) {
            ev["message"] = *message;
        }
        notificationChannel->emit(ev);
    }

    void emitLog(std::string_view level,
                 const Json &data,
                 std::optional<std::string> logger = std::nullopt) const {
        if (!notificationChannel || cancellation.isCancelled()) {
            return;
        }
        int msgSev = serve_detail::log_level_severity(level);
        if (msgSev < 0) {
            return;
        }
        if (minLogLevel.has_value()) {
            int minSev = serve_detail::log_level_severity(*minLogLevel);
            if (minSev < 0 || msgSev < minSev) {
                return;
            }
        }
        Json ev = json_obj();
        ev["type"] = "log";
        ev["level"] = std::string(level);
        if (logger.has_value()) {
            ev["logger"] = *logger;
        }
        ev["data"] = redact_json(data);
        notificationChannel->emit(ev);
    }
};

/** Protocol-agnostic tool result supporting typed JSON, untyped JSON, plain text, errors, and adapter extensions. */
struct ServeResult {
    bool isError{false};
    bool isTyped{false};
    bool isJson{false};
    int httpStatus{200};
    Json data;
    std::string textValue;
    std::string errorMessage;
    std::any extension;

    static ServeResult typed(Json obj) {
        ServeResult r;
        r.isTyped = true;
        r.isJson = true;
        r.data = std::move(obj);
        return r;
    }

    static ServeResult json(Json val) {
        ServeResult r;
        r.isTyped = false;
        r.isJson = true;
        r.data = std::move(val);
        return r;
    }

    static ServeResult text(std::string s) {
        ServeResult r;
        r.isJson = false;
        r.textValue = std::move(s);
        return r;
    }

    static ServeResult error(std::string msg, int status = 400) {
        ServeResult r;
        r.isError = true;
        r.httpStatus = status;
        r.errorMessage = std::move(msg);
        return r;
    }

    template <typename T>
    static ServeResult withExtension(T ext, ServeResult base = ServeResult::json(json_obj())) {
        base.extension = std::make_any<T>(std::move(ext));
        return base;
    }

    Json toJson() const {
        if (isError) {
            Json j = json_obj();
            j["error"] = errorMessage;
            return j;
        }
        if (isJson) {
            return data;
        }
        return Json(textValue);
    }

    std::string toDisplayText() const {
        if (isError) {
            return errorMessage;
        }
        if (isJson) {
            return slimr::write_json(data);
        }
        return textValue;
    }
};

struct ToolAnnotations {
    std::optional<bool> readOnlyHint;
    std::optional<bool> destructiveHint;
    std::optional<bool> idempotentHint;
    std::optional<bool> openWorldHint;

    Json toJson() const {
        Json j = json_obj();
        if (readOnlyHint.has_value())
            j["readOnlyHint"] = *readOnlyHint;
        if (destructiveHint.has_value())
            j["destructiveHint"] = *destructiveHint;
        if (idempotentHint.has_value())
            j["idempotentHint"] = *idempotentHint;
        if (openWorldHint.has_value())
            j["openWorldHint"] = *openWorldHint;
        return j;
    }
};

struct ToolOptions {
    ToolAnnotations annotations;
    bool threadSafe{false};
    bool runsLocal{false};
    std::vector<std::string> requiredScopes;
};

struct ToolDef {
    std::string name;
    std::string description;
    ToolAnnotations annotations;
    bool threadSafe{false};
    bool runsLocal{false};
    std::vector<std::string> requiredScopes;
    Json inputSchema;
    std::optional<Json> outputSchema;
    std::function<ServeResult(const Json &, const RequestContext &)> handler;
};

enum class ConcurrencyPolicy {
    Serialized,
    Parallel
};

struct RequiredEnvVar {
    std::string name;
    std::string purpose;
};

struct OpenApiOptions {
    bool oauthEnabled{false};
    std::string serverUrl;
};

class ServeApp {
  public:
    using AuthorizerFn = std::function<bool(const Principal &, std::string_view operation, std::string &outError)>;
    using HttpRunnerFn = std::function<int(ServeApp &, int argc, char **argv)>;
    using RemoteRelayFn = std::function<int(ServeApp &, const ToolDef &, const Json &args, const RequestContext &ctx, int argc, char **argv)>;

    std::string key;
    std::string version = "1.0.0";
    std::string description;
    std::string instructions;
    std::string bugsinkDsn;
    std::vector<RequiredEnvVar> requiredEnvVars;
    ConcurrencyPolicy concurrency{ConcurrencyPolicy::Serialized};
    std::vector<std::string> defaultRequiredScopes;
    std::vector<std::string> localScopes{"*"};
    AuthorizerFn authorizer;
    bool allowCliTools{true};
    int apiVersion{0};
    std::string remote;
    std::string remoteEnvVar;
    OpenApiOptions openApiDefaults{};
    std::function<void(std::stop_token)> onServe;

    ServeApp() : runtime_(std::make_shared<RuntimeState>()) {}
    ServeApp(ServeApp &&) noexcept = default;
    ServeApp &operator=(ServeApp &&) noexcept = default;
    explicit ServeApp(std::string appKey, std::string appVer = "1.0.0")
        : key(std::move(appKey)), version(std::move(appVer)), runtime_(std::make_shared<RuntimeState>()) {}

    void setBugsink(std::string dsn, std::string envVar = "BUGSINK_DSN", std::string release = "") {
        bugsinkDsn = dsn;
        if (release.empty() && !key.empty() && !version.empty()) {
            release = key + "@" + version;
        }
        runtime_->bugsink = std::make_shared<BugsinkReporter>(std::move(dsn), std::move(envVar), std::move(release));
    }

    BugsinkReporter *bugsink() const noexcept {
        return runtime_ ? runtime_->bugsink.get() : nullptr;
    }

    std::mutex &execMutex() const {
        return runtime_->execMutex;
    }

    void setHttpRunner(HttpRunnerFn runner) {
        httpRunner_ = std::move(runner);
    }

    bool hasHttpRunner() const noexcept { return static_cast<bool>(httpRunner_); }

    const HttpRunnerFn &httpRunner() const noexcept { return httpRunner_; }

    void setRemoteRelay(RemoteRelayFn relay) {
        remoteRelay_ = std::move(relay);
    }

    bool hasRemoteRelay() const noexcept { return static_cast<bool>(remoteRelay_); }

    const RemoteRelayFn &remoteRelay() const noexcept { return remoteRelay_; }

    ServeApp &requireEnv(std::string name, std::string purpose) {
        requiredEnvVars.push_back(RequiredEnvVar{std::move(name), std::move(purpose)});
        return *this;
    }

    template <typename InputStruct, typename Handler>
    ServeApp &tool(std::string name, std::string desc, ToolOptions opts, Handler &&h) {
        ToolDef def;
        def.name = name;
        def.description = std::move(desc);
        def.annotations = std::move(opts.annotations);
        def.threadSafe = opts.threadSafe;
        def.runsLocal = opts.runsLocal;
        def.requiredScopes = std::move(opts.requiredScopes);
        def.inputSchema = schema<InputStruct>();

        using InvokeRet = typename std::conditional_t<
            std::invocable<Handler &, const InputStruct &, const RequestContext &>,
            std::invoke_result<Handler &, const InputStruct &, const RequestContext &>,
            std::invoke_result<Handler &, const InputStruct &>>::type;
        using PlainRet = std::remove_cvref_t<InvokeRet>;
        constexpr bool kIsTypedOutput =
            !std::is_same_v<PlainRet, ServeResult> &&
            !std::is_same_v<PlainRet, std::string> &&
            !std::is_same_v<PlainRet, std::string_view> &&
            !std::is_same_v<PlainRet, const char *> &&
            !std::is_same_v<PlainRet, Json> &&
            !std::is_void_v<PlainRet>;
        if constexpr (kIsTypedOutput) {
            def.outputSchema = schema<PlainRet>();
        }

        std::weak_ptr<RuntimeState> weakRt = runtime_;
        def.handler = [weakRt, toolName = name, h = std::forward<Handler>(h)](
                          const Json &args_json, const RequestContext &ctx) mutable -> ServeResult {
            InputStruct input{};
            if constexpr (!std::is_same_v<InputStruct, std::monostate>) {
                std::string serialized_args = slimr::write_json(args_json);
                auto ec = glz::read_json(input, serialized_args);
                if (ec) {
                    return ServeResult::error("Invalid input parameters: " + glz::format_error(ec, serialized_args), 400);
                }
            }

            if (ctx.cancellation.isCancelled()) {
                throw ServeException(ServeErrorCode::RequestCancelled, "Request cancelled", Json{}, 499);
            }

            try {
                auto invoke = [&]() -> decltype(auto) {
                    if constexpr (std::invocable<Handler &, const InputStruct &, const RequestContext &>) {
                        return h(input, ctx);
                    } else {
                        return h(input);
                    }
                };
                using Ret = decltype(invoke());
                if constexpr (std::is_same_v<Ret, ServeResult>) {
                    return invoke();
                } else if constexpr (std::is_same_v<Ret, std::string> || std::is_same_v<Ret, std::string_view> || std::is_same_v<Ret, const char *>) {
                    return ServeResult::text(std::string(invoke()));
                } else if constexpr (std::is_same_v<Ret, Json>) {
                    return ServeResult::json(invoke());
                } else if constexpr (std::is_void_v<Ret>) {
                    invoke();
                    return ServeResult::json(json_obj());
                } else {
                    auto res = invoke();
                    std::string serialized = slimr::write_json(res);
                    auto parsed = parse_json<Json>(serialized);
                    if (!parsed) {
                        return ServeResult::error("Failed to serialize typed output", 500);
                    }
                    return ServeResult::typed(std::move(*parsed));
                }
            } catch (const ServeException &) {
                throw;
            } catch (const std::exception &e) {
                std::string safeMsg = redact_sensitive(e.what());
                if (auto rt = weakRt.lock()) {
                    if (rt->bugsink) {
                        Json extra = json_obj();
                        extra["tool"] = toolName;
                        rt->bugsink->reportError(std::runtime_error(safeMsg), extra);
                    }
                }
                return ServeResult::error(safeMsg, 500);
            } catch (...) {
                std::runtime_error unk("Unknown exception during tool execution");
                if (auto rt = weakRt.lock()) {
                    if (rt->bugsink) {
                        Json extra = json_obj();
                        extra["tool"] = toolName;
                        rt->bugsink->reportError(unk, extra);
                    }
                }
                return ServeResult::error(unk.what(), 500);
            }
        };

        tools_[name] = std::move(def);
        return *this;
    }

    template <typename InputStruct, typename Handler>
    ServeApp &tool(std::string name, std::string desc, ToolAnnotations annotations, Handler &&h) {
        return tool<InputStruct>(std::move(name), std::move(desc), ToolOptions{.annotations = std::move(annotations)}, std::forward<Handler>(h));
    }

    template <typename InputStruct, typename Handler>
    ServeApp &tool(std::string name, std::string desc, Handler &&h) {
        return tool<InputStruct>(std::move(name), std::move(desc), ToolOptions{}, std::forward<Handler>(h));
    }

    const std::map<std::string, ToolDef> &tools() const noexcept { return tools_; }

    ServeApp &registerTool(ToolDef def) {
        tools_[def.name] = std::move(def);
        return *this;
    }

    /** Finds a registered tool by exact name or underscore/hyphen normalization. */
    const ToolDef *findTool(std::string_view toolName) const {
        std::string exact(toolName);
        if (auto it = tools_.find(exact); it != tools_.end()) {
            return &it->second;
        }
        std::string alt = exact;
        std::replace(alt.begin(), alt.end(), '_', '-');
        if (auto it = tools_.find(alt); it != tools_.end()) {
            return &it->second;
        }
        alt = exact;
        std::replace(alt.begin(), alt.end(), '-', '_');
        if (auto it = tools_.find(alt); it != tools_.end()) {
            return &it->second;
        }
        return nullptr;
    }

  private:
    struct RuntimeState {
        std::shared_ptr<BugsinkReporter> bugsink;
        std::mutex execMutex;
    };

    std::map<std::string, ToolDef> tools_;
    std::shared_ptr<RuntimeState> runtime_;
    HttpRunnerFn httpRunner_;
    RemoteRelayFn remoteRelay_;
};

/** Generates an OpenAPI 3.1.0 document from ServeApp's registered tools and auth configuration. */
inline Json build_openapi_spec(const ServeApp &app, const OpenApiOptions &opts = {}) {
    Json spec = json_obj();
    spec["openapi"] = "3.1.0";

    Json info = json_obj();
    info["title"] = app.key.empty() ? "app" : app.key;
    info["version"] = app.version.empty() ? "1.0.0" : app.version;
    if (!app.description.empty()) {
        info["description"] = app.description;
    }
    if (app.apiVersion > 0) {
        info["x-slimr-api-version"] = app.apiVersion;
    }
    spec["info"] = std::move(info);

    if (!opts.serverUrl.empty()) {
        Json serverEntry = json_obj();
        serverEntry["url"] = opts.serverUrl;
        Json::array_t servers;
        servers.push_back(std::move(serverEntry));
        spec["servers"] = std::move(servers);
    }

    Json strType = json_obj();
    strType["type"] = "string";
    Json intType = json_obj();
    intType["type"] = "integer";

    Json errProps = json_obj();
    errProps["error"] = strType;
    errProps["message"] = strType;
    errProps["error_description"] = strType;
    errProps["code"] = intType;
    errProps["serverApiVersion"] = intType;
    errProps["clientApiVersion"] = intType;

    Json errSchema = json_obj();
    errSchema["type"] = "object";
    errSchema["required"] = Json::array_t{Json("error")};
    errSchema["properties"] = std::move(errProps);

    Json schemas = json_obj();
    schemas["ErrorResponse"] = std::move(errSchema);

    Json components = json_obj();
    components["schemas"] = std::move(schemas);

    if (opts.oauthEnabled) {
        Json bearer = json_obj();
        bearer["type"] = "http";
        bearer["scheme"] = "bearer";
        Json secSchemes = json_obj();
        secSchemes["bearerAuth"] = std::move(bearer);
        components["securitySchemes"] = std::move(secSchemes);

        Json secReq = json_obj();
        secReq["bearerAuth"] = Json::array_t{};
        Json::array_t secArr;
        secArr.push_back(std::move(secReq));
        spec["security"] = std::move(secArr);
    }
    spec["components"] = std::move(components);

    auto makeErrorResponse = [](std::string_view desc) {
        Json refObj = json_obj();
        refObj["$ref"] = "#/components/schemas/ErrorResponse";
        Json appJson = json_obj();
        appJson["schema"] = std::move(refObj);
        Json content = json_obj();
        content["application/json"] = std::move(appJson);
        Json resp = json_obj();
        resp["description"] = std::string(desc);
        resp["content"] = std::move(content);
        return resp;
    };

    Json paths = json_obj();
    for (const auto &[name, def] : app.tools()) {
        (void)name;
        Json postOp = json_obj();
        postOp["operationId"] = def.name;
        if (!def.description.empty()) {
            postOp["description"] = def.description;
        }

        Json reqJson = json_obj();
        reqJson["schema"] = def.inputSchema;
        Json reqContent = json_obj();
        reqContent["application/json"] = std::move(reqJson);
        Json reqBody = json_obj();
        reqBody["required"] = false;
        reqBody["content"] = std::move(reqContent);
        postOp["requestBody"] = std::move(reqBody);

        Json okJson = json_obj();
        okJson["schema"] = def.outputSchema.value_or(json_obj());
        Json okContent = json_obj();
        okContent["application/json"] = std::move(okJson);
        Json okResp = json_obj();
        okResp["description"] = "Tool result";
        okResp["content"] = std::move(okContent);

        Json responses = json_obj();
        responses["200"] = std::move(okResp);
        responses["400"] = makeErrorResponse("Bad request");
        if (opts.oauthEnabled || app.authorizer || !app.defaultRequiredScopes.empty() || !def.requiredScopes.empty()) {
            responses["401"] = makeErrorResponse("Unauthorized");
            responses["403"] = makeErrorResponse("Forbidden");
        }
        responses["413"] = makeErrorResponse("Payload too large");
        responses["415"] = makeErrorResponse("Unsupported media type");
        responses["426"] = makeErrorResponse("Upgrade required");
        responses["500"] = makeErrorResponse("Internal server error");
        responses["503"] = makeErrorResponse("Service unavailable");
        postOp["responses"] = std::move(responses);

        Json pathItem = json_obj();
        pathItem["post"] = std::move(postOp);
        paths["/" + def.name] = std::move(pathItem);
    }
    spec["paths"] = std::move(paths);

    return spec;
}

} // namespace slimr
