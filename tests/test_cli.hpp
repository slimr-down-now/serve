#pragma once

#include <sstream>
#include <string>
#include <vector>

namespace serve_cli_test {

struct EmptyInput {};

struct ArgvBuilder {
    std::vector<std::string> storage;
    std::vector<char *> ptrs;

    explicit ArgvBuilder(std::initializer_list<std::string> args) : storage(args) {
        ptrs.reserve(storage.size());
        for (auto &s : storage) {
            ptrs.push_back(s.data());
        }
    }

    int argc() const noexcept { return static_cast<int>(ptrs.size()); }
    char **argv() noexcept { return ptrs.data(); }
};

} // namespace serve_cli_test

// Keep TEST_CASE line numbers distinct from test_auth.hpp.
TEST_CASE("[cli] --version, --help, -h, and --list output") {
    slimr::ServeApp app("demo-cli", "1.2.3");
    app.setHttpRunner([](slimr::ServeApp &, int, char **) { return 0; });
    app.tool<serve_cli_test::EmptyInput>("ping", "Ping tool", [](const serve_cli_test::EmptyInput &) {
        return std::string("pong");
    });

    {
        serve_cli_test::ArgvBuilder args{"demo-cli", "--version"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        CHECK(out.str() == "demo-cli 1.2.3\n");
        CHECK(err.str().empty());
    }

    {
        serve_cli_test::ArgvBuilder args{"demo-cli", "--help"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        CHECK(out.str().find("serve|http") != std::string::npos);
        CHECK(out.str().find("<tool>") == std::string::npos);
        CHECK(out.str().find("--openapi") == std::string::npos);
        CHECK(out.str().find("--remote") == std::string::npos);
        CHECK(out.str().find("Available tools:") != std::string::npos);
        CHECK(out.str().find("ping - Ping tool") != std::string::npos);
    }

    {
        serve_cli_test::ArgvBuilder args{"demo-cli", "-h"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        CHECK(out.str().find("Available tools:") != std::string::npos);
    }

    {
        serve_cli_test::ArgvBuilder args{"demo-cli", "--list"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        CHECK(out.str().find("Available tools:") != std::string::npos);
    }
}

TEST_CASE("[cli] RequiredEnvVar and --skip-checks") {
    slimr::ServeApp app("env-app", "0.1.0");
    app.requireEnv("SLIMR_SERVE_MISSING_TEST_ENV_VAR_XYZ", "Needed for test");
    app.setHttpRunner([](slimr::ServeApp &, int, char **) { return 0; });

    {
        serve_cli_test::ArgvBuilder args{"env-app", "serve"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 1);
        CHECK(err.str().find("SLIMR_SERVE_MISSING_TEST_ENV_VAR_XYZ") != std::string::npos);
    }

    {
        serve_cli_test::ArgvBuilder args{"env-app", "--skip-checks", "serve"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        CHECK(err.str().empty());
    }
}

TEST_CASE("[cli] rejects tool names, --openapi, --remote, and --bearer-env") {
    slimr::ServeApp app("srv-only", "0.1.0");
    app.setHttpRunner([](slimr::ServeApp &, int, char **) { return 0; });
    app.tool<serve_cli_test::EmptyInput>("ping", "Ping", [](const serve_cli_test::EmptyInput &) {
        return std::string("pong");
    });

    for (const char *bad : {"ping", "--openapi", "--remote", "--bearer-env", "--tools"}) {
        serve_cli_test::ArgvBuilder args{"srv-only", bad};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 1);
        CHECK(err.str().find(std::string("Unknown command: ") + bad) != std::string::npos);
        CHECK(err.str().find("Usage:") != std::string::npos);
        CHECK(out.str().empty());
    }
}

TEST_CASE("[cli] serve and http subcommands delegate to httpRunner") {
    slimr::ServeApp app("srv-app", "0.1.0");
    {
        serve_cli_test::ArgvBuilder args{"srv-app", "serve"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 1);
        CHECK(err.str().find("HTTP transport is not enabled") != std::string::npos);
    }

    std::string seenSubcmd;
    app.setHttpRunner([&seenSubcmd](slimr::ServeApp &, int argc, char **argv) {
        if (argc >= 2) {
            seenSubcmd = argv[1];
        }
        return 0;
    });

    {
        serve_cli_test::ArgvBuilder args{"srv-app", "serve", "--port", "10009"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        CHECK(seenSubcmd == "serve");
    }
    {
        serve_cli_test::ArgvBuilder args{"srv-app", "http", "--port", "10009"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        CHECK(seenSubcmd == "http");
    }
}
