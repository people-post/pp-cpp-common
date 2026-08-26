# pp-cpp-common

Shared C++ foundation for People Post apps (`namespace pp`).

## Contents

- Logging (`pp::logging`)
- `Module`, `Error` / `Roe`, `ResultOrError`
- `WorkerPool`, `SequencedTaskRunner`
- `CivilTime`, `Utilities`
- Binary wire helpers (`Serialize`, `BinaryPack`)

Public headers live under `include/common/` so consumers keep `#include "common/…"`.

## Build

```bash
cmake -S . -B build -DPP_COMMON_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Consume (FetchContent)

Pin a **release tag cut from `main`** (do not track `develop` or floating branch tips):

```cmake
include(FetchContent)
FetchContent_Declare(
  pp_cpp_common
  GIT_REPOSITORY https://github.com/people-post/pp-cpp-common.git
  GIT_TAG v0.1.0
)
FetchContent_MakeAvailable(pp_cpp_common)
target_link_libraries(your_target PUBLIC pp_common)
```

Release flow: land on `develop` → merge to `main` → tag `vX.Y.Z` on `main`.
