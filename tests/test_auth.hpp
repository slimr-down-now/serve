#pragma once

TEST_CASE("[auth] extract_bearer_token parses valid headers and rejects malformed ones") {
    using slimr::serve_detail::extract_bearer_token;

    CHECK(extract_bearer_token("Bearer abc.def.ghi") == std::optional<std::string>("abc.def.ghi"));
    CHECK(extract_bearer_token("  bearer   tok123  ") == std::optional<std::string>("tok123"));
    CHECK_FALSE(extract_bearer_token("Basic dXNlcjpwYXNz").has_value());
    CHECK_FALSE(extract_bearer_token("Bearer ").has_value());
    CHECK_FALSE(extract_bearer_token("Bearer tok with space").has_value());
    CHECK_FALSE(extract_bearer_token("Bearer tok\r\n").has_value());
}

TEST_CASE("[auth] build_www_authenticate quotes params and redacts secrets") {
    std::vector<std::string> scopes = {"read", "write"};
    std::string hdr = slimr::serve_detail::build_www_authenticate(
        "https://example.test/.well-known/oauth-protected-resource",
        "insufficient_scope",
        scopes,
        "Bearer secret-tok \"quoted\"");
    CHECK(hdr.find("resource_metadata=\"https://example.test/.well-known/oauth-protected-resource\"") != std::string::npos);
    CHECK(hdr.find("error=\"insufficient_scope\"") != std::string::npos);
    CHECK(hdr.find("scope=\"read write\"") != std::string::npos);
    CHECK(hdr.find("secret-tok") == std::string::npos);
    CHECK(hdr.find("\\\"quoted\\\"") != std::string::npos);
}

TEST_CASE("[auth] evaluate_bearer_auth enforces validator, expiry, issuer, audience, and default scopes") {
    slimr::OAuthResourceConfig cfg;
    cfg.enabled = true;
    cfg.expectedIssuers = {"https://issuer.test"};
    cfg.defaultScopes = {"api:read"};

    slimr::HttpRouteRequest req;
    req.headers["authorization"] = "Bearer good-token";

    // Missing validator -> ServiceUnavailable
    auto noVal = slimr::serve_detail::evaluate_bearer_auth(cfg, "https://res.test", req);
    CHECK(noVal.status == slimr::TokenValidationResult::Status::ServiceUnavailable);

    // Query access_token rejected
    cfg.validateToken = [](const slimr::TokenValidationContext &ctx) {
        slimr::TokenValidationResult r;
        r.status = slimr::TokenValidationResult::Status::Valid;
        r.principal.authenticated = true;
        r.principal.subject = std::string(ctx.token);
        r.principal.issuer = "https://issuer.test";
        r.principal.audience = std::string(ctx.expectedResource);
        r.principal.scopes = {"api:read"};
        return r;
    };
    slimr::HttpRouteRequest queryReq = req;
    queryReq.queryParams["access_token"] = "leak";
    auto qRes = slimr::serve_detail::evaluate_bearer_auth(cfg, "https://res.test", queryReq);
    CHECK(qRes.status == slimr::TokenValidationResult::Status::InvalidToken);

    // Valid token succeeds
    auto okRes = slimr::serve_detail::evaluate_bearer_auth(cfg, "https://res.test", req);
    CHECK(okRes.status == slimr::TokenValidationResult::Status::Valid);
    CHECK(okRes.principal.subject == "good-token");

    // Insufficient default scope
    cfg.defaultScopes = {"api:read", "api:write"};
    auto insuff = slimr::serve_detail::evaluate_bearer_auth(cfg, "https://res.test", req);
    CHECK(insuff.status == slimr::TokenValidationResult::Status::InsufficientScope);
    CHECK(insuff.requiredScopes == std::vector<std::string>{"api:write"});

    // Expired token
    cfg.defaultScopes = {"api:read"};
    cfg.validateToken = [](const slimr::TokenValidationContext &ctx) {
        slimr::TokenValidationResult r;
        r.status = slimr::TokenValidationResult::Status::Valid;
        r.principal.authenticated = true;
        r.principal.issuer = "https://issuer.test";
        r.principal.audience = std::string(ctx.expectedResource);
        r.principal.scopes = {"api:read"};
        r.principal.expiresAt = std::chrono::system_clock::now() - std::chrono::seconds(5);
        return r;
    };
    auto expRes = slimr::serve_detail::evaluate_bearer_auth(cfg, "https://res.test", req);
    CHECK(expRes.status == slimr::TokenValidationResult::Status::InvalidToken);

    // Issuer / audience mismatch
    cfg.validateToken = [](const slimr::TokenValidationContext &) {
        slimr::TokenValidationResult r;
        r.status = slimr::TokenValidationResult::Status::Valid;
        r.principal.authenticated = true;
        r.principal.issuer = "https://wrong-issuer.test";
        r.principal.audience = "https://res.test";
        r.principal.scopes = {"api:read"};
        return r;
    };
    CHECK(slimr::serve_detail::evaluate_bearer_auth(cfg, "https://res.test", req).status ==
          slimr::TokenValidationResult::Status::InvalidToken);

    cfg.validateToken = [](const slimr::TokenValidationContext &) {
        slimr::TokenValidationResult r;
        r.status = slimr::TokenValidationResult::Status::Valid;
        r.principal.authenticated = true;
        r.principal.issuer = "https://issuer.test";
        r.principal.audience = "https://wrong-res.test";
        r.principal.scopes = {"api:read"};
        return r;
    };
    CHECK(slimr::serve_detail::evaluate_bearer_auth(cfg, "https://res.test", req).status ==
          slimr::TokenValidationResult::Status::InvalidToken);
}

TEST_CASE("[auth] local_principal and checkAuthorization unify scope, wildcard, and authorizer guards") {
    slimr::ServeApp app("demo", "0.1.0");
    app.defaultRequiredScopes = {"base:read"};

    // Default local_principal has authenticated=true, subject="local", scopes={"*"}
    auto local = slimr::local_principal(app);
    CHECK(local.authenticated);
    CHECK(local.subject == "local");
    CHECK(local.scopes == std::vector<std::string>{"*"});

    // Wildcard "*" satisfies both defaultRequiredScopes and toolScopes
    std::vector<std::string> toolScopes = {"admin:write"};
    CHECK_FALSE(slimr::checkAuthorization(app, local, toolScopes, "tool/admin").has_value());

    // Narrowing localScopes enforces scopes on local/socket principal
    app.localScopes = {"base:read"};
    auto deniedScope = slimr::checkAuthorization(app, slimr::local_principal(app), toolScopes, "tool/admin");
    REQUIRE(deniedScope.has_value());
    if (deniedScope.has_value()) {
        CHECK(deniedScope->httpStatus == 403);
        CHECK(deniedScope->code == slimr::ServeErrorCode::InsufficientScope);
        CHECK(deniedScope->requiredScopes == std::vector<std::string>{"admin:write"});
    }

    // Authorizer runs even when wildcard "*" satisfies scopes
    app.localScopes = {"*"};
    app.authorizer = [](const slimr::Principal &p, std::string_view op, std::string &err) {
        if (p.subject == "local" && op == "tool/blocked") {
            err = "Blocked for local";
            return false;
        }
        return true;
    };
    auto deniedAuth = slimr::checkAuthorization(app, slimr::local_principal(app), toolScopes, "tool/blocked");
    REQUIRE(deniedAuth.has_value());
    if (deniedAuth.has_value()) {
        CHECK(deniedAuth->httpStatus == 403);
        CHECK(deniedAuth->message == "Blocked for local");
    }
    CHECK_FALSE(slimr::checkAuthorization(app, slimr::local_principal(app), toolScopes, "tool/ok").has_value());

    // Unauthenticated principal (auth disabled on TCP) passes checkAuthorization
    slimr::Principal anon;
    CHECK_FALSE(slimr::checkAuthorization(app, anon, toolScopes, "tool/blocked").has_value());
}
