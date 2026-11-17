# Changelog

## Unreleased

## v0.3.0 - 2026-11-17

- Default HTTP bind host is now `127.0.0.1` (was `0.0.0.0`); pass `--host 0.0.0.0` to expose.

## v0.2.0 - 2026-11-17

- Generate OpenAPI 3.1.0 specs (`build_openapi_spec`), serve unauthenticated `GET /openapi.json`, and dump via `--openapi`.

## v0.1.1 - 2026-11-16

- Initial release: `serve_core.hpp`, `serve_cli.hpp`, `serve_auth.hpp`, `serve_http.hpp`, `serve_daemon.hpp`, and `serve_relay.hpp`.
- `serve`/`http` accept `--socket`, `--socket-mode` and `--socket-group` and serve the same routes on TCP and the Unix socket; `enable_serve` defaults take `.socketPath`, `.socketMode` and `.socketGroup`, and each listener is opt-in (`ServeHttpOptions::port` is now `std::optional<int>`).
- The Unix socket runs tool calls as the local principal (`app.localScopes`) without OAuth; `HttpRouteRequest::unixSocket` tells custom routes which listener they're on.
- `app.onServe` runs under `serve`; its stop is requested before the shutdown drain, and an escaping exception exits 1.
- Bad `serve` flag values exit 2 with a usage line; `--host` must be an IP literal.
- Internal: drop the `slimr-down-now/curl` registry and move `slimr-bugsink` to 0.1.2.
