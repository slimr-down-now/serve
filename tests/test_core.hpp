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
