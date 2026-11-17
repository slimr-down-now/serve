#pragma once
#include "serve_core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace slimr {

namespace cli_detail {

inline std::string effective_remote_env_var(const ServeApp &app) {
    if (!app.remoteEnvVar.empty()) {
        return app.remoteEnvVar;
    }
    if (app.key.empty()) {
        return {};
    }
    std::string name;
    name.reserve(app.key.size() + 7);
    for (unsigned char c : app.key) {
        if (c == '-') {
            name.push_back('_');
        } else {
            name.push_back(static_cast<char>(std::toupper(c)));
        }
    }
    name += "_REMOTE";
    return name;
}

inline bool has_env_value(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    std::string key(name);
    const char *val = std::getenv(key.c_str());
    return val != nullptr && val[0] != '\0';
}

inline std::optional<std::string> read_cli_input(std::string_view arg,
                                                 std::istream &in,
                                                 std::string &outError) {
    if (arg == "-") {
        std::ostringstream ss;
        ss << in.rdbuf();
        if (in.bad()) {
            outError = "Failed to read tool arguments from stdin";
            return std::nullopt;
        }
        return ss.str();
    }
    if (arg.starts_with('@')) {
        std::string_view pathSv = arg.substr(1);
        if (pathSv.empty()) {
            outError = "Invalid @path argument: empty file path";
            return std::nullopt;
        }
        std::filesystem::path filePath(pathSv);
        std::error_code ec;
        if (!std::filesystem::exists(filePath, ec) || ec) {
            outError = "Failed to open argument file: " + std::string(pathSv);
            return std::nullopt;
        }
        if (!std::filesystem::is_regular_file(filePath, ec) || ec) {
            outError = "Invalid @path argument: not a regular file: " + std::string(pathSv);
            return std::nullopt;
        }
        std::ifstream file(filePath, std::ios::binary);
        if (!file) {
            outError = "Failed to open argument file: " + std::string(pathSv);
            return std::nullopt;
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        if (file.bad()) {
            outError = "Failed to read argument file: " + std::string(pathSv);
            return std::nullopt;
        }
        return ss.str();
    }
    return std::string(arg);
}

inline bool unwrap_cli_envelope(const Json &top,
                                const ToolDef &def,
                                Json &outArgs,
                                RequestContext &ctx,
                                std::string &outError) {
    if (!top.is_object()) {
        outError = "Invalid JSON arguments: expected a JSON object";
        return false;
    }

    const Json::object_t *schemaProps = nullptr;
    if (def.inputSchema.is_object() && def.inputSchema.contains("properties") &&
        def.inputSchema["properties"].is_object()) {
        schemaProps = &def.inputSchema["properties"].get_object();
    }
    const bool schemaHasInput = schemaProps != nullptr && schemaProps->contains("input");
    const bool schemaHasContext = schemaProps != nullptr && schemaProps->contains("context");

    bool isEnvelope = false;
    if (top.contains("context")) {
        isEnvelope = !(schemaHasContext && !top["context"].is_object());
    } else if (top.contains("input") && !schemaHasInput) {
        isEnvelope = true;
    }

    if (!isEnvelope) {
        outArgs = top;
        return true;
    }

    for (const auto &[k, v] : top.get_object()) {
        (void)v;
        if (k != "input" && k != "context") {
            outError = "Invalid CLI envelope key: " + k + " (allowed: input, context)";
            return false;
        }
    }

    if (top.contains("input")) {
        if (!top["input"].is_object()) {
            outError = "Invalid CLI envelope: 'input' must be a JSON object";
            return false;
        }
        outArgs = top["input"];
    } else {
        outArgs = json_obj();
    }

    if (top.contains("context")) {
        if (!top["context"].is_object()) {
            outError = "Invalid CLI envelope: 'context' must be a JSON object";
            return false;
        }
        for (const auto &[k, v] : top["context"].get_object()) {
            if (k != "session") {
                outError = "Invalid CLI context key: " + k + " (allowed: session)";
                return false;
            }
            if (!v.is_string()) {
                outError = "Invalid CLI context: 'session' must be a string";
                return false;
            }
            ctx.session = v.get<std::string>();
        }
    }

    return true;
}

inline std::optional<std::string> check_cli_authorization(const ServeApp &app,
                                                          const Principal &principal,
                                                          std::span<const std::string> toolScopes,
                                                          std::string_view operation) {
    if (!principal.authenticated) {
        return std::nullopt;
    }
    const bool wildcard = principal.hasScope("*");
    if (!wildcard) {
        std::vector<std::string> missing;
        for (const auto &s : app.defaultRequiredScopes) {
            if (!principal.hasScope(s)) {
                missing.push_back(s);
            }
        }
        for (const auto &s : toolScopes) {
            if (!principal.hasScope(s)) {
                missing.push_back(s);
            }
        }
        if (!missing.empty()) {
            return "Insufficient scope for " + std::string(operation);
        }
    }
    if (app.authorizer) {
        std::string errMsg;
        if (!app.authorizer(principal, operation, errMsg)) {
            return errMsg.empty() ? "Forbidden" : errMsg;
        }
    }
    return std::nullopt;
}

} // namespace cli_detail

/** Prints CLI usage and registered tools to the given output stream. */
inline void print_cli_help(const ServeApp &app, std::ostream &os = std::cout) {
    std::string prog = app.key.empty() ? "app" : app.key;
    os << "Usage:\n";
    if (app.hasHttpRunner()) {
        os << "  " << prog << " [--skip-checks] serve|http [--host <addr>] [--port <n>] [--socket <path>] [--socket-mode <octal>] [--socket-group <name>]\n";
    }
    if (app.allowCliTools) {
        os << "  " << prog << " [--skip-checks] [--remote <target>] <tool> ['{\"arg\":\"val\"}'|@path|-]\n";
    }
    os << "  " << prog << " --openapi | --version | --help\n\n";
    os << "Available tools:\n";
    for (const auto &[name, def] : app.tools()) {
        (void)name;
        os << "  " << def.name << " - " << def.description << "\n";
        os << "    Schema: " << slimr::write_json(def.inputSchema) << "\n";
    }
}

/** Runs CLI dispatch: --help/--version, env checks, serve/http runner, or tool invocation. */
inline int run_cli(ServeApp &app,
                   int argc,
                   char *argv[],
                   std::ostream &out = std::cout,
                   std::ostream &err = std::cerr,
                   std::istream &in = std::cin) {
    if (!app.bugsinkDsn.empty() && !app.bugsink()) {
        app.setBugsink(app.bugsinkDsn);
    }

    bool skipChecks = false;
    bool hasRemoteFlag = false;
    std::vector<char *> filteredArgv;
    filteredArgv.reserve(static_cast<size_t>(argc > 0 ? argc : 1));
    if (argc > 0 && argv != nullptr) {
        filteredArgv.push_back(argv[0]);
        for (int i = 1; i < argc; ++i) {
            if (argv[i] == nullptr) {
                continue;
            }
            std::string_view arg(argv[i]);
            if (arg == "--skip-checks") {
                skipChecks = true;
            } else if (arg == "--remote") {
                if (i + 1 >= argc || argv[i + 1] == nullptr) {
                    err << "Missing value for --remote\n";
                    return 1;
                }
                hasRemoteFlag = true;
                ++i;
            } else if (arg.starts_with("--remote=")) {
                hasRemoteFlag = true;
            } else if (arg == "--bearer-env") {
                if (i + 1 >= argc || argv[i + 1] == nullptr) {
                    err << "Missing value for --bearer-env\n";
                    return 1;
                }
                ++i;
            } else if (arg.starts_with("--bearer-env=")) {
                // Preserved in original argv for remoteRelay_.
            } else {
                filteredArgv.push_back(argv[i]);
            }
        }
    }
    int fArgc = static_cast<int>(filteredArgv.size());

    if (fArgc <= 1 || (fArgc == 2 && (std::strcmp(filteredArgv[1], "--help") == 0 ||
                                      std::strcmp(filteredArgv[1], "-h") == 0 ||
                                      std::strcmp(filteredArgv[1], "--list") == 0))) {
        print_cli_help(app, out);
        return 0;
    }

    if (fArgc == 2 && std::strcmp(filteredArgv[1], "--version") == 0) {
        out << (app.key.empty() ? "app" : app.key) << " " << app.version << "\n";
        return 0;
    }

    if (fArgc == 2 && std::strcmp(filteredArgv[1], "--openapi") == 0) {
        out << slimr::write_json(build_openapi_spec(app, OpenApiOptions{.oauthEnabled = app.openApiDefaults.oauthEnabled})) << "\n";
        return 0;
    }

    if (!skipChecks && !app.requiredEnvVars.empty()) {
        std::vector<const RequiredEnvVar *> missing;
        for (const auto &req : app.requiredEnvVars) {
            const char *val = std::getenv(req.name.c_str());
            if (val == nullptr || val[0] == '\0') {
                missing.push_back(&req);
            }
        }
        if (!missing.empty()) {
            err << "Missing required environment variable(s):\n";
            for (const auto *req : missing) {
                err << "  " << req->name;
                if (!req->purpose.empty()) {
                    err << ": " << req->purpose;
                }
                err << "\n";
            }
            return 1;
        }
    }

    std::string firstArg = filteredArgv[1];
    if (firstArg == "serve" || firstArg == "http") {
        if (!app.hasHttpRunner()) {
            err << "HTTP transport is not enabled in this binary (link slimr::serve_http).\n";
            return 1;
        }
        return app.httpRunner()(app, fArgc, filteredArgv.data());
    }

    if (!app.allowCliTools) {
        std::string prog = app.key.empty() ? "app" : app.key;
        err << "CLI tool calls are disabled for " << prog << "; use 'serve' or 'http'\n";
        return 1;
    }

    const ToolDef *def = app.findTool(firstArg);
    if (!def) {
        err << "Unknown tool: " << firstArg << "\n";
        print_cli_help(app, err);
        return 1;
    }

    std::string rawInput = "{}";
    if (fArgc >= 3) {
        std::string readErr;
        auto loaded = cli_detail::read_cli_input(filteredArgv[2], in, readErr);
        if (!loaded) {
            err << readErr << "\n";
            return 1;
        }
        rawInput = std::move(*loaded);
    }

    auto parsed = parse_json<Json>(rawInput);
    if (!parsed) {
        err << "Invalid JSON arguments: " << parsed.error() << "\n";
        return 1;
    }

    RequestContext cliCtx;
    cliCtx.principal.authenticated = true;
    cliCtx.principal.subject = "local";
    cliCtx.principal.scopes = app.localScopes;
    cliCtx.notificationChannel = std::make_shared<NotificationChannel>([&err](const Json &ev) {
        err << slimr::write_json(ev) << "\n";
    });

    Json args = json_obj();
    std::string envErr;
    if (!cli_detail::unwrap_cli_envelope(*parsed, *def, args, cliCtx, envErr)) {
        err << envErr << "\n";
        return 1;
    }

    const std::string remoteEnv = cli_detail::effective_remote_env_var(app);
    const bool hasRemoteTarget = hasRemoteFlag || cli_detail::has_env_value(remoteEnv) || !app.remote.empty();
    if (!def->runsLocal && hasRemoteTarget) {
        if (app.hasRemoteRelay()) {
            return app.remoteRelay()(app, *def, args, cliCtx, argc, argv);
        }
        if (hasRemoteFlag) {
            err << "Remote relay is not enabled in this binary (link slimr::serve_http).\n";
            return 1;
        }
    }

    if (auto authErr = cli_detail::check_cli_authorization(app, cliCtx.principal, def->requiredScopes, "tools/call:" + def->name)) {
        err << *authErr << "\n";
        return 1;
    }

    ServeResult res;
    try {
        if (app.concurrency == ConcurrencyPolicy::Serialized && !def->threadSafe) {
            std::lock_guard lock(app.execMutex());
            res = def->handler(args, cliCtx);
        } else {
            res = def->handler(args, cliCtx);
        }
    } catch (const std::exception &e) {
        err << redact_sensitive(e.what()) << "\n";
        return 1;
    }

    if (res.isError) {
        err << res.errorMessage << "\n";
        return 1;
    }

    if (res.isJson) {
        out << slimr::write_json(res.data) << "\n";
    } else {
        out << res.textValue << "\n";
    }
    return 0;
}

} // namespace slimr
