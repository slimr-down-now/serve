#define SLIMR_TEST_MAIN
#include "serve_yaml.hpp"
#include "test.hpp"

#include <unistd.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct TempDirGuard {
    std::filesystem::path dir;

    explicit TempDirGuard(const std::string &tag) {
        dir = std::filesystem::temp_directory_path() /
              ("slimr-serve-yaml-" + tag + "-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }

    TempDirGuard(const TempDirGuard &) = delete;
    TempDirGuard &operator=(const TempDirGuard &) = delete;

    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

struct AddIn {
    slimr::Field<int, "First"> a;
    slimr::Field<int, "Second"> b;
};

struct AddOut {
    slimr::Field<int, "Sum"> sum;
};

struct StrIn {
    slimr::Field<std::string, "Name"> name;
    slimr::OptField<int, "Count"> count;
};

struct StrOut {
    slimr::Field<std::string, "Greeting"> greeting;
};

struct EmptyIn {};

struct WorkIn {
    slimr::OptField<int, "Unused"> unused;
};

httplib::Client unix_client(const std::string &sockPath) {
    httplib::Client cli(sockPath);
    cli.set_address_family(AF_UNIX);
    return cli;
}

void register_yaml_test_tools(slimr::ServeApp &app) {
    app.tool<AddIn>(
        "add",
        "Add two integers",
        [](const AddIn &in) {
            return AddOut{.sum = in.a.value + in.b.value};
        });
    app.tool<StrIn>(
        "greet",
        "Greet by name",
        [](const StrIn &in) {
            std::string out = in.name.value;
            if (in.count.value > 0) {
                out += ":" + std::to_string(in.count.value);
            }
            return StrOut{.greeting = std::move(out)};
        });
    app.tool<WorkIn>(
        "work",
        "Stream progress",
        [](const WorkIn &, const slimr::RequestContext &ctx) {
            ctx.emitProgress(1.0, 1.0, "step");
            return AddOut{.sum = 42};
        });
    app.registerTool(slimr::ToolDef{
        .name = "echo_raw",
        .description = "Echo raw body",
        .inputSchema = slimr::schema<EmptyIn>(),
        .handler = [](const slimr::Json &args, const slimr::RequestContext &) {
            return slimr::ServeResult::json(args);
        },
    });
}

} // namespace

TEST_CASE("[yaml] explicit application/yaml and application/json over TCP and Unix socket") {
    TempDirGuard tmp("explicit");
    auto sockPath = (tmp.dir / "yaml.sock").string();

    slimr::ServeApp app("calc", "1.0.0");
    register_yaml_test_tools(app);

    slimr::ServeHttpOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    opts.socketPath = sockPath;
    slimr::enable_yaml(opts);

    slimr::ServeHttpServer srv(app, opts);
    std::string err;
    REQUIRE(srv.start(&err));

    httplib::Client tcp("127.0.0.1", srv.boundPort());
    auto sock = unix_client(sockPath);

    for (auto *c : {&tcp, &sock}) {
        auto res = c->Post("/add", "a: 7\nb: 5\n", "application/yaml");
        REQUIRE(res);
        CHECK(res->status == 200);
        CHECK(res->get_header_value("Content-Type") == "application/yaml");
        auto y = slimr::parse_yaml<slimr::Json>(res->body);
        REQUIRE(y.has_value());
        CHECK((*y)["sum"].get<int64_t>() == 12);

        auto resStar = c->Post("/add", {{"Accept", "*/*"}}, "a: 2\nb: 3\n", "application/yaml");
        REQUIRE(resStar);
        CHECK(resStar->status == 200);
        CHECK(resStar->get_header_value("Content-Type") == "application/yaml");

        auto resJsonAccept = c->Post("/add", {{"Accept", "application/json"}}, "a: 2\nb: 3\n", "application/yaml");
        REQUIRE(resJsonAccept);
        CHECK(resJsonAccept->status == 200);
        CHECK(resJsonAccept->get_header_value("Content-Type") == "application/json");
        auto j = slimr::parse_json<slimr::Json>(resJsonAccept->body);
        REQUIRE(j.has_value());
        CHECK((*j)["sum"].get<int64_t>() == 5);
    }

    auto rawRes = tcp.Post("/echo_raw", "input:\n  msg: hi\ncontext:\n  session: s-42\n", "application/yaml");
    REQUIRE(rawRes);
    CHECK(rawRes->status == 200);
    CHECK(rawRes->get_header_value("Content-Type") == "application/yaml");
    auto rawYaml = slimr::parse_yaml<slimr::Json>(rawRes->body);
    REQUIRE(rawYaml.has_value());
    CHECK((*rawYaml)["input"]["msg"].get<std::string>() == "hi");
    CHECK((*rawYaml)["context"]["session"].get<std::string>() == "s-42");

    auto emptyRes = tcp.Post("/echo_raw", "", "application/yaml");
    REQUIRE(emptyRes);
    CHECK(emptyRes->status == 200);
    CHECK(emptyRes->get_header_value("Content-Type") == "application/yaml");
    auto emptyYaml = slimr::parse_yaml<slimr::Json>(emptyRes->body);
    REQUIRE(emptyYaml.has_value());
    CHECK(emptyYaml->empty());

    auto notFound = tcp.Post("/missing_tool", "", "application/yaml");
    REQUIRE(notFound);
    CHECK(notFound->status == 404);
    CHECK(notFound->get_header_value("Content-Type") == "application/yaml");
    auto nfYaml = slimr::parse_yaml<slimr::Json>(notFound->body);
    REQUIRE(nfYaml.has_value());
    CHECK((*nfYaml)["error"].get<std::string>() == "not_found");

    auto badYaml = tcp.Post("/add", "items: [unterminated", "application/yaml");
    REQUIRE(badYaml);
    CHECK(badYaml->status == 400);
    CHECK(badYaml->get_header_value("Content-Type") == "application/yaml");
    auto badYamlBody = slimr::parse_yaml<slimr::Json>(badYaml->body);
    REQUIRE(badYamlBody.has_value());
    CHECK((*badYamlBody)["error"].get<std::string>() == "invalid_yaml");

    // Explicit application/json skips YAML fallback even for valid YAML flow/block syntax.
    auto strictJsonBlock = tcp.Post("/add", "a: 1\nb: 2\n", "application/json");
    REQUIRE(strictJsonBlock);
    CHECK(strictJsonBlock->status == 400);
    CHECK(strictJsonBlock->get_header_value("Content-Type") == "application/json");
    auto sj1 = slimr::parse_json<slimr::Json>(strictJsonBlock->body);
    REQUIRE(sj1.has_value());
    CHECK((*sj1)["error"].get<std::string>() == "invalid_json");

    auto strictJsonFlow = tcp.Post("/add", {{"Accept", "application/yaml"}}, "{a: 1, b: 2}", "application/json");
    REQUIRE(strictJsonFlow);
    CHECK(strictJsonFlow->status == 400);
    CHECK(strictJsonFlow->get_header_value("Content-Type") == "application/yaml");
    auto sj2 = slimr::parse_yaml<slimr::Json>(strictJsonFlow->body);
    REQUIRE(sj2.has_value());
    CHECK((*sj2)["error"].get<std::string>() == "invalid_json");

    srv.stop();
}

TEST_CASE("[yaml] BOM/whitespace detection, JSON-to-YAML fallback, and Accept precedence over TCP and Unix socket") {
    TempDirGuard tmp("detect");
    auto sockPath = (tmp.dir / "detect.sock").string();

    slimr::ServeApp app("calc", "1.0.0");
    register_yaml_test_tools(app);

    slimr::ServeHttpOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    opts.socketPath = sockPath;
    slimr::enable_yaml(opts);

    slimr::ServeHttpServer srv(app, opts);
    REQUIRE(srv.start());

    httplib::Client tcp("127.0.0.1", srv.boundPort());
    auto sock = unix_client(sockPath);

    for (const std::string &ct : {std::string(""), std::string("application/x-www-form-urlencoded"), std::string("text/plain")}) {
        auto jsonRes = tcp.Post("/add", R"({"a":3,"b":4})", ct);
        REQUIRE(jsonRes);
        CHECK(jsonRes->status == 200);
        CHECK(jsonRes->get_header_value("Content-Type") == "application/json");
        auto j = slimr::parse_json<slimr::Json>(jsonRes->body);
        REQUIRE(j.has_value());
        CHECK((*j)["sum"].get<int64_t>() == 7);

        auto bomJsonRes = tcp.Post("/add", "\xEF\xBB\xBF  {\"a\":3,\"b\":4}", ct);
        REQUIRE(bomJsonRes);
        CHECK(bomJsonRes->status == 200);
        CHECK(bomJsonRes->get_header_value("Content-Type") == "application/json");

        // Leading '{' invalid as JSON falls back to YAML flow mapping.
        for (auto *c : {&tcp, &sock}) {
            auto flowRes = c->Post("/add", "{a: 3, b: 4}", ct);
            REQUIRE(flowRes);
            CHECK(flowRes->status == 200);
            CHECK(flowRes->get_header_value("Content-Type") == "application/yaml");
            auto y = slimr::parse_yaml<slimr::Json>(flowRes->body);
            REQUIRE(y.has_value());
            CHECK((*y)["sum"].get<int64_t>() == 7);
        }

        // Leading '{' invalid in both JSON and YAML reports the JSON error.
        auto bothBad = tcp.Post("/add", "{bad", ct);
        REQUIRE(bothBad);
        CHECK(bothBad->status == 400);
        CHECK(bothBad->get_header_value("Content-Type") == "application/json");
        auto bj = slimr::parse_json<slimr::Json>(bothBad->body);
        REQUIRE(bj.has_value());
        CHECK((*bj)["error"].get<std::string>() == "invalid_json");

        auto bothBadYamlAccept = tcp.Post("/add", {{"Accept", "application/yaml"}}, "{bad", ct);
        REQUIRE(bothBadYamlAccept);
        CHECK(bothBadYamlAccept->status == 400);
        CHECK(bothBadYamlAccept->get_header_value("Content-Type") == "application/yaml");
        auto by = slimr::parse_yaml<slimr::Json>(bothBadYamlAccept->body);
        REQUIRE(by.has_value());
        CHECK((*by)["error"].get<std::string>() == "invalid_json");

        // Non-'{'/'[' body parses as YAML (with BOM + whitespace stripped).
        auto blockRes = tcp.Post("/add", "\xEF\xBB\xBF  a: 3\n  b: 4\n", ct);
        REQUIRE(blockRes);
        CHECK(blockRes->status == 200);
        CHECK(blockRes->get_header_value("Content-Type") == "application/yaml");

        auto badBlock = tcp.Post("/add", "items: [unterminated", ct);
        REQUIRE(badBlock);
        CHECK(badBlock->status == 400);
        CHECK(badBlock->get_header_value("Content-Type") == "application/yaml");
        auto bb = slimr::parse_yaml<slimr::Json>(badBlock->body);
        REQUIRE(bb.has_value());
        CHECK((*bb)["error"].get<std::string>() == "invalid_yaml");
    }

    // When Accept lists both application/json and application/yaml, JSON wins.
    auto bothAccept = tcp.Post("/add", {{"Accept", "application/json, application/yaml"}}, "a: 1\nb: 2\n", "application/yaml");
    REQUIRE(bothAccept);
    CHECK(bothAccept->status == 200);
    CHECK(bothAccept->get_header_value("Content-Type") == "application/json");

    srv.stop();
}

TEST_CASE("[yaml] NDJSON precedence, /openapi.json note, scalar coercion, and non-opt-in 415") {
    slimr::ServeApp app("calc", "1.0.0");
    app.description = "Calculator tools.";
    register_yaml_test_tools(app);

    slimr::ServeHttpOptions yamlOpts;
    yamlOpts.host = "127.0.0.1";
    yamlOpts.port = 0;
    slimr::enable_yaml(yamlOpts);

    slimr::ServeHttpServer yamlSrv(app, yamlOpts);
    REQUIRE(yamlSrv.start());
    httplib::Client yamlCli("127.0.0.1", yamlSrv.boundPort());

    // NDJSON streaming wins over application/yaml in Accept and YAML request body.
    auto streamRes = yamlCli.Post("/work", {{"Accept", "application/x-ndjson, application/yaml"}}, "unused: 1\n", "application/yaml");
    REQUIRE(streamRes);
    CHECK(streamRes->status == 200);
    CHECK(streamRes->get_header_value("Content-Type").find("application/x-ndjson") != std::string::npos);
    std::vector<slimr::Json> events;
    std::istringstream iss(streamRes->body);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty())
            continue;
        auto parsed = slimr::parse_json<slimr::Json>(line);
        REQUIRE(parsed.has_value());
        events.push_back(*parsed);
    }
    REQUIRE(events.size() == 2);
    CHECK(events[0]["type"].get<std::string>() == "progress");
    CHECK(events[1]["type"].get<std::string>() == "result");
    CHECK(events[1]["data"]["sum"].get<int64_t>() == 42);

    // /openapi.json stays JSON and appends the YAML capability note.
    auto oaRes = yamlCli.Get("/openapi.json");
    REQUIRE(oaRes);
    CHECK(oaRes->status == 200);
    CHECK(oaRes->get_header_value("Content-Type") == "application/json");
    auto oa = slimr::parse_json<slimr::Json>(oaRes->body);
    REQUIRE(oa.has_value());
    CHECK((*oa)["info"]["description"].get<std::string>() ==
          "Calculator tools.\n\nEndpoints also accept and return application/yaml.");

    // Scalar coercion: quoted "123" binds std::string, unquoted 123 fails, null/comment fail.
    auto quotedStr = yamlCli.Post("/greet", "name: \"123\"\ncount: 4\n", "application/yaml");
    REQUIRE(quotedStr);
    CHECK(quotedStr->status == 200);
    auto qy = slimr::parse_yaml<slimr::Json>(quotedStr->body);
    REQUIRE(qy.has_value());
    CHECK((*qy)["greeting"].get<std::string>() == "123:4");

    auto unquotedStr = yamlCli.Post("/greet", "name: 123\n", "application/yaml");
    REQUIRE(unquotedStr);
    CHECK(unquotedStr->status == 400);
    CHECK(unquotedStr->get_header_value("Content-Type") == "application/yaml");

    auto emptyScalar = yamlCli.Post("/add", "a: \nb: 2\n", "application/yaml");
    REQUIRE(emptyScalar);
    CHECK(emptyScalar->status == 400);
    CHECK(emptyScalar->get_header_value("Content-Type") == "application/yaml");

    auto commentOnly = yamlCli.Post("/add", "# just a comment\n", "application/yaml");
    REQUIRE(commentOnly);
    CHECK(commentOnly->status == 400);
    CHECK(commentOnly->get_header_value("Content-Type") == "application/yaml");
    auto cy = slimr::parse_yaml<slimr::Json>(commentOnly->body);
    REQUIRE(cy.has_value());
    CHECK((*cy)["error"].get<std::string>() == "invalid_params");

    yamlSrv.stop();

    // Non-opt-in server stays JSON-only: 415 on application/yaml and form-urlencoded, no OpenAPI note.
    slimr::ServeHttpOptions plainOpts;
    plainOpts.host = "127.0.0.1";
    plainOpts.port = 0;
    slimr::ServeHttpServer plainSrv(app, plainOpts);
    REQUIRE(plainSrv.start());
    httplib::Client plainCli("127.0.0.1", plainSrv.boundPort());

    auto plainYaml = plainCli.Post("/add", "a: 1\nb: 2\n", "application/yaml");
    REQUIRE(plainYaml);
    CHECK(plainYaml->status == 415);

    auto plainForm = plainCli.Post("/add", R"({"a":1,"b":2})", "application/x-www-form-urlencoded");
    REQUIRE(plainForm);
    CHECK(plainForm->status == 415);

    auto plainOa = plainCli.Get("/openapi.json");
    REQUIRE(plainOa);
    CHECK(plainOa->status == 200);
    auto poa = slimr::parse_json<slimr::Json>(plainOa->body);
    REQUIRE(poa.has_value());
    CHECK((*poa)["info"]["description"].get<std::string>() == "Calculator tools.");

    plainSrv.stop();
}
