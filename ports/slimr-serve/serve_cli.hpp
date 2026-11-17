#pragma once
#include "serve_core.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace slimr {

/** Prints CLI usage and registered tools to the given output stream. */
inline void print_cli_help(const ServeApp &app, std::ostream &os = std::cout) {
    std::string prog = app.key.empty() ? "app" : app.key;
    os << "Usage:\n";
    if (app.hasHttpRunner()) {
        os << "  " << prog << " [--skip-checks] serve|http [--host <addr>] [--port <n>] [--socket <path>] [--socket-mode <octal>] [--socket-group <name>]\n";
    }
    os << "  " << prog << " --version | --help\n\n";
    os << "Available tools:\n";
    for (const auto &[name, def] : app.tools()) {
        (void)name;
        os << "  " << def.name << " - " << def.description << "\n";
        os << "    Schema: " << slimr::write_json(def.inputSchema) << "\n";
    }
}

/** Runs CLI dispatch: --help/--version, env checks, and serve/http runner. */
inline int run_cli(ServeApp &app,
                   int argc,
                   char *argv[],
                   std::ostream &out = std::cout,
                   std::ostream &err = std::cerr,
                   std::istream &in = std::cin) {
    (void)in;
    if (!app.bugsinkDsn.empty() && !app.bugsink()) {
        app.setBugsink(app.bugsinkDsn);
    }

    bool skipChecks = false;
    std::vector<char *> filteredArgv;
    filteredArgv.reserve(static_cast<size_t>(argc > 0 ? argc : 1));
    if (argc > 0 && argv != nullptr) {
        filteredArgv.push_back(argv[0]);
        for (int i = 1; i < argc; ++i) {
            if (argv[i] == nullptr) {
                continue;
            }
            if (std::string_view(argv[i]) == "--skip-checks") {
                skipChecks = true;
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

    err << "Unknown command: " << firstArg << "\n";
    print_cli_help(app, err);
    return 1;
}

} // namespace slimr
