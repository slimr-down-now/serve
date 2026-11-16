# slimr-serve

Header-only typed tool servers, JSON CLIs, and daemon lifecycle primitives for C++26

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

Declare `slimr-serve` in `vcpkg.json` (add the `"http"` feature for `ServeHttpServer`, `DaemonServer`, and CLI remote relay) and link `slimr::serve` / `slimr::serve_http` in `CMakeLists.txt`:

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

## Build & Test

```bash
just test            # Debug build (ASan + UBSan) and run tests
just lint            # Check formatting and run clang-tidy
just release patch   # Trigger release workflow and pull tag commit
```

Extra test targets go in `CMakeLists.txt` (`add_executable` and `add_test`): unfiltered `just test` runs all via `ctest`, while `just test "<case>"` runs only `test_target`.

## Environment Variables

| Name | Required | Purpose |
| --- | --- | --- |
| *(none)* | — | — |
