# Module: Export/ABI Macros (`FeatherExport`)

## Purpose

`FeatherExport.h` defines visibility and language-linkage macros used by all public headers.

Main file:
- `System/FeatherExport.h`

## Public Macros

- `FEATHER_EXTERN_C_BEGIN`
- `FEATHER_EXTERN_C_END`
- `FEATHER_API`

## Behavior

- `FEATHER_EXTERN_C_BEGIN/END` expand to `extern "C" { ... }` in C++ and no-op in C.
- `FEATHER_API` controls symbol export/import:
  - empty for static builds (`FEATHER_STATIC_DEFINE`)
  - `__declspec(dllexport/dllimport)` on Windows
  - `__attribute__((visibility("default")))` on GCC/Clang

## Usage Pattern

Public Feather headers wrap exported declarations with these macros so the same headers work for:
- static linking
- shared-library consumers
- C and C++ callers
