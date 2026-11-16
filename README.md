# slimr-serve

Header-only typed tool servers and JSON CLIs

## Install

Add `slimr-down-now/<repo>` to `vcpkg-configuration.json`:

```json
{
  "kind": "git",
  "repository": "https://github.com/slimr-down-now/<repo>",
  "baseline": "<commit-sha>",
  "packages": [
    "slimr-serve"
  ]
}
```

Declare `slimr-serve` in `vcpkg.json` and link `slimr-serve::slimr-serve` in `CMakeLists.txt`:

```cmake
find_package(slimr-serve CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE slimr-serve::slimr-serve)
```

If your library needs one implementation TU (like rapidyaml), see `slimr-yaml`'s `INTERFACE_SOURCES` pattern in `ports/slimr-yaml/slimr-yaml-config.cmake`.

## Usage

```cpp
#include <slimr-serve.hpp>

int sum = lib::add(2, 3);
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
