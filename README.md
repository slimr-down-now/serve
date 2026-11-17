# slimr-serve

Header-only typed tool servers and daemon lifecycle primitives for C++26

## Install

Add `slimr-down-now/serve` to `vcpkg-configuration.json`:

```json
{
  "kind": "git",
  "repository": "https://github.com/slimr-down-now/serve",
  "baseline": "<commit-sha>",
  "packages": [
    "slimr-serve"
  ]
}
```

Declare `slimr-serve` in `vcpkg.json` (add the `"http"` feature for `ServeHttpServer` and `DaemonServer`) and link `slimr::serve` / `slimr::serve_http` in `CMakeLists.txt`:

```cmake
find_package(slimr-serve CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE slimr::serve)
# With the "http" feature enabled:
target_link_libraries(my_target PRIVATE slimr::serve_http)
```

## Usage

```cpp
#include <serve.hpp>
#include <serve_http.hpp> // when using the "http" feature

struct AddIn {
    slimr::Field<int, "First operand"> a;
    slimr::Field<int, "Second operand"> b;
};

struct AddOut {
    slimr::Field<int, "Sum"> sum;
};

int main(int argc, char **argv) {
    slimr::ServeApp app("calc", "0.1.0");
    app.tool<AddIn>("add", "Add two numbers", [](const AddIn &in) {
        return AddOut{.sum = in.a.value + in.b.value};
    });
    slimr::enable_serve(app, {.host = "127.0.0.1", .port = 10001});
    return slimr::run_cli(app, argc, argv);
}
```

### Serving

`calc serve` (alias `calc http`) serves the same routes on every listener: `POST /<tool>` (raw tool input JSON, or NDJSON progress when `Accept: application/x-ndjson`), `GET /openapi.json` (OpenAPI 3.1.0), `GET /schema`, `GET /health`, plus any `customRoutes` registered first. Each listener is opt-in; with neither configured, `serve` exits 1.

`ServeHttpOptions::toolRoutes` (default `true`) controls whether `/schema`, `/openapi.json`, and `POST /:tool` are mounted, and `ServeHttpOptions::toolsPrefix` (empty by default, e.g. `"/tools"`) mounts them under a prefix instead of root. `OAuthResourceConfig::expectedResource` overrides the `baseUrl()` audience/resource value passed to `validateToken`.

```bash
calc serve --host 127.0.0.1 --port 0              # TCP; port 0 binds an ephemeral port and logs the URL
calc serve --socket /path/run/calc.sock           # Unix socket only
calc serve --port 10001 --socket /path/run/calc.sock --socket-mode 0660 --socket-group staff
```

Invoke tools over HTTP or a Unix socket with `curl`:

```bash
curl -sS -X POST http://127.0.0.1:10001/add -H 'Content-Type: application/json' -d '{"a":1,"b":2}'
curl -sS --unix-socket /path/run/calc.sock -X POST http://localhost/add -H 'Content-Type: application/json' -d '{}'
```

| Flag | Default | Purpose |
| --- | --- | --- |
| `--host <addr>` | `127.0.0.1` | TCP bind address (IP literal; pass `0.0.0.0` to expose) |
| `--port <n>` | app default | Enables the TCP listener |
| `--socket <path>` | app default | Enables the Unix socket listener |
| `--socket-mode <octal>` | `0600` | Socket file mode |
| `--socket-group <name>` | none | Socket file group (parent dir becomes `0750`, else `0700`) |

Flags override the defaults passed to `enable_serve` (`.host`, `.port`, `.socketPath`, `.socketMode`, `.socketGroup`). TCP evaluates bearer/OAuth when `oauth.enabled`; the socket runs as the local principal (`app.localScopes`) with no OAuth. Both go through the same scope checks and `app.authorizer`. A second `serve` on the same socket exits 1 (`flock` on `<socket>.lock`); a stale socket file is replaced and the socket is unlinked on stop.

`app.onServe` runs on its own thread once the listeners are up. On SIGINT/SIGTERM its `std::stop_token` is signaled before in-flight calls drain, then the thread is joined. An exception escaping `onServe` is reported (redacted, to stderr and Bugsink) and shuts the server down with exit 1.

## Build & Test

```bash
just test            # Debug build (ASan + UBSan) and run tests
just lint            # Check formatting and run clang-tidy
```

Extra test targets go in `CMakeLists.txt` (`add_executable` and `add_test`): unfiltered `just test` runs all via `ctest`, while `just test "<case>"` runs only `test_target`.

See [docs/contributing.md](docs/contributing.md) for local builds, testing, unreleased-library overlays, and releases.

## Environment Variables

| Name | Required | Purpose |
| --- | --- | --- |
| *(none)* | — | — |
