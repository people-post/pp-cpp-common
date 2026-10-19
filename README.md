# pp-cpp-common

Shared C++ foundation for People Post apps (`namespace pp`).

## Contents

- Logging (`pp::logging`)
- `Module`, `Error` / `Roe`, `ResultOrError`
- `WorkerPool`, `SequencedTaskRunner`
- `Executor` (where tasks run: `ThreadExecutor`, `WorkerLaneExecutor`, `SequencedTaskRunner`) — libraries take the executors they run on from the app
- `SerialExecutor` (one thread: `PostAfter` timers, `RunAndWait` for lifecycle edges) — `ThreadExecutor`, and `ManualExecutor` for deterministic tests (a queue and a clock the test drives)
- `ValueJson` (parse / dump / typed reads / deep merge over `Value`), `AtomicFileWrite` (temp + fsync + rename; owner-only on POSIX), `SettledWait` (one-shot cross-thread result), `CodedFailure` (a layer's coded error enum in `Roe`), `MetricsRegistry` (counters / gauges / histograms, Prometheus text)
- `Lifetime` — keeps posted work from reaching an object that stopped or went away (`Bind` / `Invalidate`; `InvalidateAndWait` across threads)
- `CivilTime`, `Utilities`
- Binary wire helpers (`Serialize`, `BinaryPack`)
- Human document tree: `Value` / `Object`, `FiFoMap`
- Hand-written UTF-8 JSON IO (`common/io/Json.h`)

Public headers live under `include/common/` so consumers keep `#include "common/…"`.
(`Meta` remains a ledger-only typedef shim over `Object`.)

## Build

```bash
cmake -S . -B build -DPP_COMMON_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

CI (`.github/workflows/build.yml`) builds and tests on Ubuntu 24.04, Windows 2022, and macOS 14 for every PR to `develop` / `main`.

## Consume (FetchContent)

Pin a **release tag cut from `main`** (do not track `develop` or floating branch tips):

```cmake
include(FetchContent)
FetchContent_Declare(
  pp_cpp_common
  GIT_REPOSITORY https://github.com/people-post/pp-cpp-common.git
  GIT_TAG v1.0.0
)
FetchContent_MakeAvailable(pp_cpp_common)
target_link_libraries(your_target PUBLIC pp_common)
```

Release flow: land on `develop` → merge to `main` → tag `vX.Y.Z` on `main`.
