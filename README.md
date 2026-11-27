# pp-cpp-common

Shared C++ foundation for People Post apps (`namespace pp`).

## Contents

- Logging (`pp::logging`)
- `Module`, `Error` / `Roe`, `ResultOrError`
- `WorkerPool`, `SequencedTaskRunner`
- `Executor` (where tasks run: `ThreadExecutor`, `WorkerLaneExecutor`, `SequencedTaskRunner`) — libraries take the executors they run on from the app
- `SerialExecutor` (one thread: `PostAfter` timers, `RunAndWait` for lifecycle edges) — `ThreadExecutor`, and `ManualExecutor` for deterministic tests (a queue and a clock the test drives)
- `ValueJson` (parse / dump / typed reads / deep merge over `Value`), `AtomicFileWrite` (a temp of its own + fsync + rename, or `Sync::None` without the fsync; owner-only on POSIX; Windows waits briefly for an open handle), `SettledWait` (one-shot cross-thread result), `CodedFailure` (a layer's coded error enum in `Roe`), `MetricsRegistry` (counters / gauges / histograms, Prometheus text)
- `Lifetime` — keeps posted work from reaching an object that stopped or went away (`Bind` / `Invalidate`; `InvalidateAndWait` across threads)
- **Process runtime** (`common/runtime/`): `AppRuntime` — worker pool, coordinator (mailbox + timer wheel), UI mailbox, a teardown gate (`QuiesceForTeardown`), and **owner threads** the app registers (`AppRuntimeConfig::owners`: id + thread name; Dedicated, or Manual for tests). On an owner: `OwnerExecutor` / `RuntimeOwnerExecutor` (where a runner runs), `OwnerTasks` (one object's posts and timers, dropped with it), `OwnerOutbox` (a child's typed report to its parent), `OwnerSteps` (continuations waiting for an event); `QueueOwnerExecutor` for tests. `PP_ASSERT_ON_OWNER` checks affinity in debug builds
- `CivilTime`, `Utilities`
- Binary wire helpers (`Serialize`, `BinaryPack`)
- `ExtArea` — the signed extension area (pp-browser `SIGNED_FORMATS.md` § 1.2, RC020): the last field of every signed object's signed bytes, `u64 BE length ‖ entries`, each entry `u16 BE type ‖ u32 BE length ‖ value`, bit 15 of the type = critical, strictly ascending types, at most 4 KiB of entries; decode (whole buffer, embedded at an offset, or the entries alone for JSON's `ext_b64`), encode, and the "unknown critical entry" query
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

CI (`.github/workflows/build.yml`) builds and tests on Ubuntu 24.04, Windows 2022, and macOS 14 for every PR to `main`.

### Frozen vectors

[`ext_area_vectors.json`](https://github.com/people-post/pp-specs/blob/main/vectors/ext_area_vectors.json) is the extension area's cross-implementation set (SIGNED_FORMATS § 6). It lives in [pp-specs](https://github.com/people-post/pp-specs/blob/main/vectors/README.md), which every implementation (this library, pp-browser, the relay) reads at a pinned tag: the tests fetch it ([`cmake/PpSpecs.cmake`](cmake/PpSpecs.cmake); `-DPP_SPECS_SOURCE_DIR=<checkout>` for a local one). Its shape is described at the top of [`tests/ext_area_test.cpp`](tests/ext_area_test.cpp), which builds the file by hand-written bytes and fails when pp-specs' file differs. After a deliberate change (new cases only), write a fresh file and bring it to pp-specs by pull request, then move the pin:

```bash
PP_WRITE_EXT_AREA_VECTORS=/tmp/ext_area_vectors.json ./build/pp_common_test --gtest_filter='ExtAreaVectorsTest.*'
```

## Consume (FetchContent)

Pin a **release tag cut from `main`** (do not track floating branch tips such as `main`):

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

Release flow: land on `main` (the only branch; there is no `develop`) → tag `vX.Y.Z` on `main`.

## Formatting

Every first-party C/C++ source is formatted with [`.clang-format`](.clang-format) by exactly clang-format 23.1.5 (`pip install clang-format==23.1.5`; other versions format differently). Run `./scripts/check_format.sh --fix` before committing; CI fails on an unformatted file. Vendored code is left as it came.
