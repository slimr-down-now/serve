#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace serve_cli_test {

struct EchoInput {
    slimr::Field<std::string, "Message"> msg;
};

struct EchoOutput {
    slimr::Field<std::string, "Echoed message"> echoed;
    slimr::OptField<std::string, "Session id"> session;
};

struct InputFieldInput {
    slimr::Field<std::string, "Literal input field"> input;
};

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

struct TempDirGuard {
    std::filesystem::path dir;
    explicit TempDirGuard(std::string_view name) {
        dir = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(dir);
    }
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    TempDirGuard(const TempDirGuard &) = delete;
    TempDirGuard &operator=(const TempDirGuard &) = delete;
};

} // namespace serve_cli_test

TEST_CASE("[cli] --version, --help, -h, --list and allowCliTools help output") {
    slimr::ServeApp app("demo-cli", "1.2.3");
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
        CHECK(out.str().find("<tool>") != std::string::npos);
        CHECK(out.str().find("Available tools:") != std::string::npos);
        CHECK(out.str().find("ping - Ping tool") != std::string::npos);
    }

    app.allowCliTools = false;
    {
        serve_cli_test::ArgvBuilder args{"demo-cli", "--list"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        CHECK(out.str().find("<tool>") == std::string::npos);
        CHECK(out.str().find("Available tools:") != std::string::npos);
    }
}

TEST_CASE("[cli] RequiredEnvVar and --skip-checks") {
    slimr::ServeApp app("env-app", "0.1.0");
    app.requireEnv("SLIMR_SERVE_MISSING_TEST_ENV_VAR_XYZ", "Needed for test");
    app.tool<serve_cli_test::EmptyInput>("ping", "Ping", [](const serve_cli_test::EmptyInput &) {
        return std::string("pong");
    });

    {
        serve_cli_test::ArgvBuilder args{"env-app", "ping"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 1);
        CHECK(err.str().find("SLIMR_SERVE_MISSING_TEST_ENV_VAR_XYZ") != std::string::npos);
    }

    {
        serve_cli_test::ArgvBuilder args{"env-app", "--skip-checks", "ping"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        CHECK(out.str() == "pong\n");
    }
}

TEST_CASE("[cli] allowCliTools=false rejects tool calls") {
    slimr::ServeApp app("locked", "0.1.0");
    app.allowCliTools = false;
    app.tool<serve_cli_test::EmptyInput>("ping", "Ping", [](const serve_cli_test::EmptyInput &) {
        return std::string("pong");
    });

    serve_cli_test::ArgvBuilder args{"locked", "ping"};
    std::ostringstream out, err;
    std::istringstream in;
    CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 1);
    CHECK(err.str().find("CLI tool calls are disabled") != std::string::npos);
    CHECK(out.str().empty());
}

TEST_CASE("[cli] typed output JSON, @path, and stdin '-'") {
    slimr::ServeApp app("echo-app", "0.1.0");
    app.tool<serve_cli_test::EchoInput>("echo", "Echo", [](const serve_cli_test::EchoInput &in, const slimr::RequestContext &ctx) {
        ctx.emitProgress(1.0, 1.0, "working");
        serve_cli_test::EchoOutput res;
        res.echoed = in.msg.value;
        res.session.value = ctx.session.value_or("");
        return res;
    });

    // Inline JSON
    {
        serve_cli_test::ArgvBuilder args{"echo-app", "echo", R"({"msg":"hi"})"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        auto parsed = slimr::parse_json<slimr::Json>(out.str());
        REQUIRE(parsed.has_value());
        CHECK((*parsed)["echoed"].get<std::string>() == "hi");
        CHECK(err.str().find("\"type\":\"progress\"") != std::string::npos);
    }

    // Stdin '-'
    {
        serve_cli_test::ArgvBuilder args{"echo-app", "echo", "-"};
        std::ostringstream out, err;
        std::istringstream in(R"({"input":{"msg":"from-stdin"},"context":{"session":"s-42"}})");
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        auto parsed = slimr::parse_json<slimr::Json>(out.str());
        REQUIRE(parsed.has_value());
        CHECK((*parsed)["echoed"].get<std::string>() == "from-stdin");
        CHECK((*parsed)["session"].get<std::string>() == "s-42");
    }

    // @path regular file + directory + missing + empty
    serve_cli_test::TempDirGuard tmp("slimr_serve_cli_test");
    std::filesystem::path filePath = tmp.dir / "args.json";
    {
        std::ofstream f(filePath);
        f << R"({"msg":"from-file"})";
    }
    {
        std::string atArg = "@" + filePath.string();
        serve_cli_test::ArgvBuilder args{"echo-app", "echo", atArg};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        auto parsed = slimr::parse_json<slimr::Json>(out.str());
        REQUIRE(parsed.has_value());
        CHECK((*parsed)["echoed"].get<std::string>() == "from-file");
    }
    {
        std::string atDir = "@" + tmp.dir.string();
        serve_cli_test::ArgvBuilder args{"echo-app", "echo", atDir};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 1);
        CHECK(err.str().find("not a regular file") != std::string::npos);
    }
    {
        serve_cli_test::ArgvBuilder args{"echo-app", "echo", "@"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 1);
        CHECK(err.str().find("empty file path") != std::string::npos);
    }
}

TEST_CASE("[cli] envelope validation and schema disambiguation") {
    slimr::ServeApp app("env-test", "0.1.0");
    app.tool<serve_cli_test::EmptyInput>("noop", "Noop", [](const serve_cli_test::EmptyInput &, const slimr::RequestContext &ctx) {
        slimr::Json j = slimr::json_obj();
        j["session"] = ctx.session.value_or("");
        j["auth"] = ctx.principal.authenticated;
        j["subject"] = ctx.principal.subject;
        return j;
    });
    app.tool<serve_cli_test::InputFieldInput>("with-input", "Has input field", [](const serve_cli_test::InputFieldInput &in) {
        return in.input.value;
    });

    // Omitted input in envelope defaults to {}
    {
        serve_cli_test::ArgvBuilder args{"env-test", "noop", R"({"context":{"session":"abc"}})"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        auto parsed = slimr::parse_json<slimr::Json>(out.str());
        REQUIRE(parsed.has_value());
        CHECK((*parsed)["session"].get<std::string>() == "abc");
        CHECK((*parsed)["auth"].get<bool>());
        CHECK((*parsed)["subject"].get<std::string>() == "local");
    }

    // Reject unknown envelope key
    {
        serve_cli_test::ArgvBuilder args{"env-test", "noop", R"({"input":{},"extra":1})"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 1);
        CHECK(err.str().find("Invalid CLI envelope key") != std::string::npos);
    }

    // Reject principal/scopes in context
    {
        serve_cli_test::ArgvBuilder args{"env-test", "noop", R"({"context":{"principal":"admin"}})"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 1);
        CHECK(err.str().find("Invalid CLI context key") != std::string::npos);
    }

    // Disambiguate tool whose schema defines "input"
    {
        serve_cli_test::ArgvBuilder args{"env-test", "with-input", R"({"input":"literal"})"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        CHECK(out.str() == "literal\n");
    }
}

TEST_CASE("[cli] local principal scopes, authorizer, and remote relay delegation") {
    slimr::ServeApp app("auth-cli", "0.1.0");
    bool relayed = false;
    app.remote = "unix:/tmp/demo.sock";
    app.setRemoteRelay([&relayed](slimr::ServeApp &, const slimr::ToolDef &def, const slimr::Json &, const slimr::RequestContext &, int, char **) {
        relayed = true;
        return def.name == "remote_tool" ? 0 : 1;
    });
    app.tool<serve_cli_test::EmptyInput>(
        "remote_tool",
        "Relayed",
        slimr::ToolOptions{.requiredScopes = {"bus:write"}},
        [](const serve_cli_test::EmptyInput &) { return std::string("local-exec"); });
    app.tool<serve_cli_test::EmptyInput>(
        "local_tool",
        "Local only",
        slimr::ToolOptions{.runsLocal = true, .requiredScopes = {"bus:read"}},
        [](const serve_cli_test::EmptyInput &) { return std::string("local-ok"); });

    // Remote relay invoked for non-runsLocal tool
    {
        serve_cli_test::ArgvBuilder args{"auth-cli", "remote_tool"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        CHECK(relayed);
    }

    // runsLocal skips relay and uses local principal ("*")
    relayed = false;
    {
        serve_cli_test::ArgvBuilder args{"auth-cli", "local_tool"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 0);
        CHECK_FALSE(relayed);
        CHECK(out.str() == "local-ok\n");
    }

    // Narrowing localScopes enforces requiredScopes
    app.localScopes = {"other:scope"};
    {
        serve_cli_test::ArgvBuilder args{"auth-cli", "local_tool"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 1);
        CHECK(err.str().find("Insufficient scope") != std::string::npos);
    }

    // Authorizer is invoked on local principal
    app.localScopes = {"*"};
    app.authorizer = [](const slimr::Principal &, std::string_view, std::string &outErr) {
        outErr = "Denied by policy";
        return false;
    };
    {
        serve_cli_test::ArgvBuilder args{"auth-cli", "local_tool"};
        std::ostringstream out, err;
        std::istringstream in;
        CHECK(slimr::run_cli(app, args.argc(), args.argv(), out, err, in) == 1);
        CHECK(err.str().find("Denied by policy") != std::string::npos);
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
