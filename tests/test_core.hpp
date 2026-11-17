#pragma once

namespace serve_core_test {

struct AddInput {
    slimr::Field<int, "First operand"> a;
    slimr::Field<int, "Second operand"> b;
};

struct AddOutput {
    slimr::Field<int, "Sum"> sum;
};

struct PingInput {};

} // namespace serve_core_test

TEST_CASE("[core] typed handler registration generates inputSchema and outputSchema") {
    slimr::ServeApp app("calc", "0.1.0");
    app.tool<serve_core_test::AddInput>(
        "add",
        "Add two numbers",
        slimr::ToolOptions{.annotations = {.readOnlyHint = true}, .threadSafe = true, .runsLocal = true},
        [](const serve_core_test::AddInput &in) {
            return serve_core_test::AddOutput{.sum = in.a.value + in.b.value};
        });

    const auto *def = app.findTool("add");
    REQUIRE(def != nullptr);
    CHECK(def->threadSafe);
    CHECK(def->runsLocal);
    CHECK(def->inputSchema.contains("properties"));
    CHECK(def->inputSchema["properties"].contains("a"));
    REQUIRE(def->outputSchema.has_value());
    if (def->outputSchema.has_value()) {
        const auto &outSchema = *def->outputSchema;
        CHECK(outSchema.contains("properties"));
        CHECK(outSchema["properties"].contains("sum"));
    }

    slimr::RequestContext ctx;
    ctx.session = "sess-1";
    slimr::Json args = slimr::json_obj();
    args["a"] = 2;
    args["b"] = 3;
    auto res = def->handler(args, ctx);
    CHECK_FALSE(res.isError);
    CHECK(res.isTyped);
    CHECK(res.isJson);
    CHECK(res.data["sum"].get<int64_t>() == 5);
}

TEST_CASE("[core] untyped Json, string, void, and ServeResult handlers omit outputSchema") {
    slimr::ServeApp app("untyped", "0.1.0");
    app.tool<serve_core_test::PingInput>("json_tool", "Json", [](const serve_core_test::PingInput &) {
        slimr::Json j = slimr::json_obj();
        j["ok"] = true;
        return j;
    });
    app.tool<serve_core_test::PingInput>("str_tool", "Str", [](const serve_core_test::PingInput &) {
        return std::string("pong");
    });
    app.tool<serve_core_test::PingInput>("void_tool", "Void", [](const serve_core_test::PingInput &) {
    });
    app.tool<serve_core_test::PingInput>("res_tool", "Res", [](const serve_core_test::PingInput &) {
        return slimr::ServeResult::error("bad input", 422);
    });

    CHECK_FALSE(app.findTool("json_tool")->outputSchema.has_value());
    CHECK_FALSE(app.findTool("str_tool")->outputSchema.has_value());
    CHECK_FALSE(app.findTool("void_tool")->outputSchema.has_value());
    CHECK_FALSE(app.findTool("res_tool")->outputSchema.has_value());

    slimr::RequestContext ctx;
    auto jr = app.findTool("json-tool")->handler(slimr::json_obj(), ctx);
    CHECK_FALSE(jr.isTyped);
    CHECK(jr.isJson);
    CHECK(jr.data["ok"].get<bool>());

    auto sr = app.findTool("str_tool")->handler(slimr::json_obj(), ctx);
    CHECK_FALSE(sr.isJson);
    CHECK(sr.textValue == "pong");

    auto vr = app.findTool("void_tool")->handler(slimr::json_obj(), ctx);
    CHECK(vr.isJson);
    CHECK(vr.data.empty());

    auto rr = app.findTool("res_tool")->handler(slimr::json_obj(), ctx);
    CHECK(rr.isError);
    CHECK(rr.httpStatus == 422);
    CHECK(rr.errorMessage == "bad input");
}

TEST_CASE("[core] redacts sensitive text and json keys") {
    std::string raw = "Authorization: Bearer secret-tok url?access_token=abc123&safe=1";
    std::string redacted = slimr::redact_sensitive(raw);
    CHECK(redacted.find("secret-tok") == std::string::npos);
    CHECK(redacted.find("abc123") == std::string::npos);
    CHECK(redacted.find("safe=1") != std::string::npos);

    slimr::Json j = slimr::json_obj();
    j["token"] = "xyz";
    j["Authorization"] = "Bearer abc";
    j["nested"] = slimr::json_obj();
    j["nested"]["note"] = "url?api_key=mykey";
    auto rj = slimr::redact_json(j);
    CHECK(rj["token"].get<std::string>() == "[REDACTED]");
    CHECK(rj["Authorization"].get<std::string>() == "[REDACTED]");
    CHECK(rj["nested"]["note"].get<std::string>().find("mykey") == std::string::npos);
}

TEST_CASE("[core] RequestContext emits progress and filtered log events") {
    slimr::CancellationSource cancelSrc;
    std::vector<slimr::Json> events;
    auto channel = std::make_shared<slimr::NotificationChannel>([&events](const slimr::Json &ev) {
        events.push_back(ev);
    });

    slimr::RequestContext ctx;
    ctx.minLogLevel = "warning";
    ctx.cancellation = cancelSrc.token();
    ctx.notificationChannel = channel;

    ctx.emitProgress(1.0, 2.0, "halfway");
    ctx.emitLog("info", slimr::Json("ignored"));
    ctx.emitLog("error", slimr::Json("Bearer tok123"), "worker");

    REQUIRE(events.size() == 2);
    CHECK(events[0]["type"].get<std::string>() == "progress");
    CHECK(events[0]["progress"].get<double>() == 1.0);
    CHECK(events[1]["type"].get<std::string>() == "log");
    CHECK(events[1]["level"].get<std::string>() == "error");
    CHECK(events[1]["data"].get<std::string>().find("tok123") == std::string::npos);

    cancelSrc.cancel();
    ctx.emitProgress(2.0, 2.0, "done");
    CHECK(events.size() == 2);
}

TEST_CASE("[core] RequiredEnvVar and ServeApp metadata") {
    slimr::ServeApp app("demo", "2.0.0");
    app.apiVersion = 3;
    app.remote = "unix:/tmp/demo.sock";
    app.remoteEnvVar = "DEMO_REMOTE";
    app.requireEnv("DEMO_KEY", "API key");
    CHECK(app.apiVersion == 3);
    CHECK(app.localScopes == std::vector<std::string>{"*"});
    REQUIRE(app.requiredEnvVars.size() == 1);
    CHECK(app.requiredEnvVars[0].name == "DEMO_KEY");
}

TEST_CASE("[core] build_openapi_spec generates OpenAPI 3.1.0 with schemas, servers, and auth") {
    // Empty app defaults: version "1.0.0", empty paths, no servers, no bearerAuth
    {
        slimr::ServeApp emptyApp("empty", "");
        auto spec = slimr::build_openapi_spec(emptyApp);
        CHECK(spec["openapi"].get<std::string>() == "3.1.0");
        CHECK(spec["info"]["title"].get<std::string>() == "empty");
        CHECK(spec["info"]["version"].get<std::string>() == "1.0.0");
        CHECK_FALSE(spec["info"].contains("description"));
        CHECK_FALSE(spec["info"].contains("x-slimr-api-version"));
        CHECK_FALSE(spec.contains("servers"));
        CHECK_FALSE(spec.contains("security"));
        CHECK_FALSE(spec["components"].contains("securitySchemes"));
        CHECK(spec["components"]["schemas"]["ErrorResponse"]["type"].get<std::string>() == "object");
        CHECK(spec["paths"].empty());
    }

    // Typed + untyped tools, metadata, serverUrl, and auth gating
    slimr::ServeApp app("calc", "2.1.0");
    app.description = "Calculator tools";
    app.apiVersion = 4;
    app.tool<serve_core_test::AddInput>("add", "Add two numbers", [](const serve_core_test::AddInput &in) {
        return serve_core_test::AddOutput{.sum = in.a.value + in.b.value};
    });
    app.tool<serve_core_test::PingInput>("ping", "Ping", [](const serve_core_test::PingInput &) {
        return std::string("pong");
    });

    auto noAuth = slimr::build_openapi_spec(app, slimr::OpenApiOptions{.serverUrl = "http://127.0.0.1:8080"});
    CHECK(noAuth["info"]["version"].get<std::string>() == "2.1.0");
    CHECK(noAuth["info"]["description"].get<std::string>() == "Calculator tools");
    CHECK(noAuth["info"]["x-slimr-api-version"].get<int64_t>() == 4);
    REQUIRE(noAuth["servers"].size() == 1);
    CHECK(noAuth["servers"][0]["url"].get<std::string>() == "http://127.0.0.1:8080");

    const auto &addPost = noAuth["paths"]["/add"]["post"];
    CHECK(addPost["operationId"].get<std::string>() == "add");
    CHECK(addPost["description"].get<std::string>() == "Add two numbers");
    CHECK(addPost["requestBody"]["required"].get<bool>() == false);
    CHECK(addPost["requestBody"]["content"]["application/json"]["schema"]["properties"].contains("a"));
    CHECK(addPost["responses"]["200"]["content"]["application/json"]["schema"]["properties"].contains("sum"));
    CHECK(addPost["responses"]["400"]["content"]["application/json"]["schema"]["$ref"].get<std::string>() ==
          "#/components/schemas/ErrorResponse");
    CHECK_FALSE(addPost["responses"].contains("401"));
    CHECK_FALSE(addPost["responses"].contains("403"));

    const auto &pingOk = noAuth["paths"]["/ping"]["post"]["responses"]["200"]["content"]["application/json"]["schema"];
    CHECK(pingOk.is_object());
    CHECK(pingOk.empty());

    // OAuth enabled adds bearerAuth, security, and 401/403
    auto withOAuth = slimr::build_openapi_spec(app, slimr::OpenApiOptions{.oauthEnabled = true});
    CHECK(withOAuth["components"]["securitySchemes"]["bearerAuth"]["scheme"].get<std::string>() == "bearer");
    REQUIRE(withOAuth["security"].size() == 1);
    CHECK(withOAuth["security"][0].contains("bearerAuth"));
    CHECK(withOAuth["paths"]["/add"]["post"]["responses"].contains("401"));
    CHECK(withOAuth["paths"]["/add"]["post"]["responses"].contains("403"));

    // Authorizer or scopes alone add 401/403 without bearerAuth
    app.tool<serve_core_test::PingInput>(
        "scoped",
        "Scoped tool",
        slimr::ToolOptions{.requiredScopes = {"read"}},
        [](const serve_core_test::PingInput &) { return std::string("ok"); });
    auto scopedSpec = slimr::build_openapi_spec(app);
    CHECK_FALSE(scopedSpec["paths"]["/add"]["post"]["responses"].contains("401"));
    CHECK(scopedSpec["paths"]["/scoped"]["post"]["responses"].contains("401"));
    CHECK(scopedSpec["paths"]["/scoped"]["post"]["responses"].contains("403"));

    app.authorizer = [](const slimr::Principal &, std::string_view, std::string &) { return true; };
    auto authzSpec = slimr::build_openapi_spec(app);
    CHECK(authzSpec["paths"]["/add"]["post"]["responses"].contains("401"));
    CHECK(authzSpec["paths"]["/add"]["post"]["responses"].contains("403"));
}
