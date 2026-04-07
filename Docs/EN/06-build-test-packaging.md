# Module: Build, Test, and Packaging

## Purpose

This document describes project-level build options, all Make targets, installation steps, downstream CMake and pkg-config integration, and the Arduino export.

Main files:
- `Makefile`
- `CMakeLists.txt`
- `cmake/FeatherConfig.cmake.in`
- `cmake/Feather.pc.in`

## Requirements

- CMake ≥ 3.20
- A C99 compiler (GCC, Clang, MSVC)
- GNU Make (for convenience targets)

## Build System Layout

- CMake is the primary build generator.
- The Makefile wraps CMake for convenience.

Key output roots:

| Path | Contents |
|---|---|
| `build/static/` | Static-library build artifacts |
| `build/shared/` | Shared-library build artifacts |
| `build/arduino/Feather/` | Arduino-compatible library export |

## Make Targets

### Building

| Target | Description |
|---|---|
| `make` / `make build` | Configure (if needed) and build static library + all tools |
| `make shared` | Configure and build shared library + all tools |

### Testing

| Target | Description |
|---|---|
| `make check` | Build (static) then run `Feather`, `FeatherTestCLI all`, `FeatherResourceTrackingTest` |
| `make check-shared` | Same as `check` but for the shared-library build |
| `make check-memory` | Build and run `FeatherMemoryTest` |
| `make run-system-score-test` | Build and run `FeatherSystemScoreTest` |

### Individual build targets

| Target | Executable built |
|---|---|
| `make demo` | `FeatherDemo` |
| `make fast-demo` | `FeatherFastDemo` |
| `make test-cli` | `FeatherTestCLI` |
| `make memory-test` | `FeatherMemoryTest` |
| `make system-score-test` | `FeatherSystemScoreTest` |
| `make tracking-test` | `FeatherResourceTrackingTest` |

### Running individual tools

| Target | Description |
|---|---|
| `make run` | Run `Feather` (scheduler integration tests) |
| `make run-demo` | Run `FeatherDemo` |
| `make run-fast-demo` | Run `FeatherFastDemo` |
| `make run-test-cli` | Run `FeatherTestCLI all` |
| `make run-memory-test` | Run `FeatherMemoryTest` |
| `make run-tracking-test` | Run `FeatherResourceTrackingTest` |
| `make run-full-system-benchmark` | Build and run `FeatherFullSystemBenchmark` |

### Installation

```sh
# Install static library to /usr/local (default prefix)
make install

# Install shared library to /usr/local
make install-shared

# Install to a custom prefix
make install prefix=/your/install/path
make install-shared prefix=/your/install/path
```

Under the hood these run `cmake --install <build_dir> --prefix <prefix>`.

### Cleaning

| Target | Description |
|---|---|
| `make clean` | Remove all build directories |
| `make distclean` | Remove build directories and logs |

### Arduino

```sh
make arduino-export
```

Produces `build/arduino/Feather/` with:
- All sources from `System/` under `src/`
- `library.properties` metadata

## CMake Options

All options default to `ON` unless noted:

| Option | Description |
|---|---|
| `FEATHER_BUILD_SHARED` | Build shared instead of static library (default: `OFF`) |
| `FEATHER_BUILD_EXAMPLE` | Build `Feather` example executable |
| `FEATHER_BUILD_DEMO` | Build `FeatherDemo` |
| `FEATHER_BUILD_FAST_DEMO` | Build `FeatherFastDemo` |
| `FEATHER_BUILD_TEST_CLI` | Build `FeatherTestCLI` |
| `FEATHER_BUILD_MEMORY_TEST` | Build `FeatherMemoryTest` (requires `FEATHER_BUILD_RESOURCE_TRACKING`) |
| `FEATHER_BUILD_SYSTEM_SCORE_TEST` | Build `FeatherSystemScoreTest` |
| `FEATHER_BUILD_RESOURCE_TRACKING` | Build `libFeatherResourceTracking` extension |
| `FEATHER_BUILD_RESOURCE_TRACKING_TEST` | Build `FeatherResourceTrackingTest` |
| `FEATHER_INSTALL_PKGCONFIG` | Install `Feather.pc` for pkg-config |

Example — static library only, no tools:

```sh
cmake -S . -B build/minimal \
  -DFEATHER_BUILD_EXAMPLE=OFF \
  -DFEATHER_BUILD_DEMO=OFF \
  -DFEATHER_BUILD_FAST_DEMO=OFF \
  -DFEATHER_BUILD_TEST_CLI=OFF \
  -DFEATHER_BUILD_MEMORY_TEST=OFF \
  -DFEATHER_BUILD_SYSTEM_SCORE_TEST=OFF \
  -DFEATHER_BUILD_RESOURCE_TRACKING_TEST=OFF
cmake --build build/minimal
```

## Installed Artifacts

| Artifact | Install path |
|---|---|
| `libFeather.a` / `.so` / `.dylib` | `<prefix>/lib/` |
| `libFeatherResourceTracking.*` (optional) | `<prefix>/lib/` |
| `Feather.h`, `FeatherRuntime/*.h`, `FeatherExport.h` | `<prefix>/include/` |
| `FeatherConfig.cmake`, `FeatherConfigVersion.cmake`, `FeatherTargets.cmake` | `<prefix>/lib/cmake/Feather/` |
| `Feather.pc` (optional) | `<prefix>/lib/pkgconfig/` |

## Consuming from CMake

After installation:

```cmake
find_package(Feather REQUIRED)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE Feather::Feather)
```

With the optional resource-tracking extension:

```cmake
target_link_libraries(my_app PRIVATE Feather::FeatherResourceTracking)
```

If installed to a non-standard prefix, pass `CMAKE_PREFIX_PATH`:

```sh
cmake -DCMAKE_PREFIX_PATH=/your/install/path -S . -B build
```

## Consuming via pkg-config

```sh
# Compiler and linker flags
pkg-config --cflags --libs Feather

# In a Makefile
CFLAGS  += $(shell pkg-config --cflags Feather)
LDFLAGS += $(shell pkg-config --libs   Feather)
```

If installed to a non-standard prefix, set `PKG_CONFIG_PATH`:

```sh
export PKG_CONFIG_PATH=/your/install/path/lib/pkgconfig:$PKG_CONFIG_PATH
```
