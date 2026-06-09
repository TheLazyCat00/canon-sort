# Canon Sort

A fast, adaptive hybrid sort for arrays of any fixed-width numeric type. Canon sort combines recursive bucket partitioning, pattern detection, and optional TBB-based parallelism to handle a wide range of input distributions efficiently.

> **Prior art notice.** This repository is published as prior art. The canon sort algorithm — including its adaptive bucket partitioning, fixed-point range mapping, ping-pong scratch buffer design, and sortedness early-exit scan — is hereby dedicated to the public domain of prior art as of the first commit date of this repository.

## Features

- Supports all standard integer and floating-point types (`i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`, `f32`, `f64`)
- C++ indirect pointer sorting helpers for heap objects via cached numeric key extraction
- Adaptive strategy selection: early exits for sorted/reversed/uniform data, radix fallback for clustered distributions
- Parallel recursion via Intel TBB (`tbb::task_group`) with work-stealing scheduler
- Single scratch buffer allocation — no per-level allocations, no unnecessary copies
- Zero dispatch overhead on typed entry points via `__asm__` symbol aliasing

## Performance

Benchmarked against hand-written quicksort, LSD radix sort, pdqsort (`std::sort`), and IPS4o (parallel). Median of 20 runs, 3 warmup rounds.

| n | Uniform | Gaussian | Sorted | RevSorted | PipeOrgan | FewVals |
|---|---------|----------|--------|-----------|-----------|---------|
| 1,000 | **fastest** | **fastest** | **fastest** | **fastest** | **fastest** | **fastest** |
| 10,000 | **fastest** | **fastest** | **fastest** | **fastest** | **fastest** | **fastest** |
| 100,000 | **fastest** | **fastest** | 2nd | **fastest** | **fastest** | **fastest** |
| 1,000,000 | **fastest** | **fastest** | 2nd | **fastest** | **fastest** | 2nd |
| 5,000,000 | 2nd | 2nd | 2nd | **fastest** | **fastest** | 2nd |

In the current benchmark run, canon sort is fastest on all six tested distributions at 1k and 10k, on five of six at 100k, on four of six at 1M, and remains top-two everywhere at 5M. IPS4o still wins the largest fully parallel-friendly cases (notably uniform, gaussian, sorted, and few-values at 5M), while canon sort stays strongest on reverse-sorted and pipe-organ inputs.

## Requirements

- Intel TBB (`tbb::task_group`)

## Building

Canon sort uses [`just`](https://github.com/casey/just) as its build system and [`zig`](https://ziglang.org/) as the compiler/cross-compilation toolchain.

### Host build (shared library)

```bash
just build        # builds libcanon.a, links against shared libtbb.so
```

### Benchmark

```bash
just bench        # builds TBB, compiles bench binary, runs it immediately
```

### Cross-compilation

Canon sort can be cross-compiled to static and shared libraries for multiple targets using Zig's built-in cross-compilation. TBB is compiled directly from source for each target (no CMake needed) and merged into the output archive.

```bash
just cross x86_64-linux-gnu     # single target
just cross-all                  # all targets
```

Supported Linux targets: `x86_64-linux-gnu`, `x86_64-linux-musl`, `aarch64-linux-gnu`, `aarch64-linux-musl`.

Supported Windows targets: `x86_64-windows-gnu`, `aarch64-windows-gnu`.

Each Linux target produces two files in `dist/<target>/`:
- `libcanon.a` — static archive with TBB merged in (self-contained)
- `libcanon.so` — shared library

Each Windows target produces:
- `libcanon.lib` — static archive with TBB merged in (self-contained)
- `libcanon.dll` — shared library

### Cleaning

```bash
just clean        # removes build/, dist/, libcanon.a (keeps vendored TBB builds)
just clean-all    # full clean including TBB builds
```

## Usage

### Typed entry points (recommended)

```cpp
#include "canon_sort.hpp"

int32_t data[] = { 5, 3, 8, 1, 9, 2 };
canon_sort_i32(data, 6);

float fdata[] = { 3.14f, 1.41f, 2.72f };
canon_sort_f32(fdata, 3);
```

### Generic dispatcher

```cpp
#include "canon_sort.hpp"

void *ptr = get_buffer();
int   n   = get_count();
canon_sort(ptr, n, CANON_U32);  // CANON_U8, CANON_U16, CANON_U32, CANON_U64
```

The generic dispatcher takes the element size in bytes (use the `CANON_U*` macros as a convenience — they equal `sizeof` for each unsigned type) and routes to the corresponding sort. It does not handle signed or floating-point types — use the typed entry points for those. The dispatcher exists for callers where the concrete type isn't known at compile time, such as plugin systems or FFI boundaries.

### All typed entry points

```cpp
void canon_sort_i8 (void *ptr, int n);
void canon_sort_i16(void *ptr, int n);
void canon_sort_i32(void *ptr, int n);
void canon_sort_i64(void *ptr, int n);
void canon_sort_u8 (void *ptr, int n);
void canon_sort_u16(void *ptr, int n);
void canon_sort_u32(void *ptr, int n);
void canon_sort_u64(void *ptr, int n);
void canon_sort_f32(void *ptr, int n);
void canon_sort_f64(void *ptr, int n);
```

### Indirect sorting of heap objects (C++)

If your objects live on the heap, keep an array of pointers and sort the pointer array by a cached numeric key.

```cpp
#include "canon_sort.hpp"

struct Entity {
    uint32_t score;
    // ... other fields ...
};

std::vector<Entity *> ptrs = load_entities();
canon_sort_ptrs_by(ptrs.data(), (int)ptrs.size(),
    [](const Entity &e) { return e.score; });
```

If you already have keys precomputed, use the cached-key helper directly:

```cpp
std::vector<Entity *> ptrs = load_entities();
std::vector<uint32_t> keys(ptrs.size());
for (size_t i = 0; i < ptrs.size(); i++) keys[i] = ptrs[i]->score;

canon_sort_ptrs_by_cached_keys(ptrs.data(), keys.data(), (int)ptrs.size());
```

These helpers reorder only the pointer array; the pointed-to heap objects stay in place. Key types must be one of the same fixed-width numeric types supported by canon sort itself. Equal-key order is unspecified (the indirect sort is not stable).

### Indirect pointer benchmark

```bash
just bench-indirect   # builds and runs the heap-object pointer benchmark
```

A sample run is saved in `bench/results_indirect.txt`. The indirect benchmark now includes three baselines:
- `std::sort (objs)` — reorders full objects
- `std::sort (ptrs)` — reorders only pointers
- `Canon Sort (ptrs)` — reorders only pointers after caching numeric keys once

In the current 1M-element run, canon sort's cached-key pointer path is about **7.0×–12.0×** faster than `std::sort (objs)` for the cheap-key case, and about **8.1×–8.6×** faster for the expensive-key case. Against `std::sort (ptrs)`, it is about **7.3×–10.8×** faster with cheap keys and about **10.6×–11.4×** faster with expensive keys.

## Notes

- NaN behaviour is undefined for `f32`/`f64`
- Thread parallelism is bounded by `log2(hardware_concurrency × 8)` depth and only activates for buckets with ≥ 16,384 elements

## License

The **specification** (`spec.md`) is released under [CC0 1.0 Universal](LICENSES/CC0-1.0.txt) — public domain, no rights reserved. The algorithm design, complexity analysis, and design decisions described therein may be freely used by anyone for any purpose.

The **implementation** (`src/`, `include/`, `bench/bench.cpp`) is released under the [MIT License](LICENSES/MIT.txt).

The benchmark harness includes [IPS4o](https://github.com/ips4o/ips4o) as a comparison target, which is licensed under BSD 2-Clause — see `bench/ips4o/LICENSE`.

This split is intentional: the idea is public domain prior art, the code is permissively licensed.
