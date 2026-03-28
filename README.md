# Canon sort

A fast, adaptive hybrid sort for arrays of any fixed-width numeric type. Canon sort combines recursive bucket partitioning, pattern detection, and optional TBB-based parallelism to handle a wide range of input distributions efficiently.

## Features

- Supports all standard integer and floating-point types (`i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`, `f32`, `f64`)
- Adaptive strategy selection: early exits for sorted/reversed/uniform data, radix fallback for clustered distributions
- Parallel recursion via Intel TBB (`tbb::task_group`) with work-stealing scheduler
- Single scratch buffer allocation — no per-level allocations, no unnecessary copies
- Zero dispatch overhead on typed entry points via `__asm__` symbol aliasing

## Performance

Benchmarked against hand-written quicksort, LSD radix sort, pdqsort (`std::sort`), and IPS4o (parallel). Median of 20 runs, 3 warmup rounds.

| n | Uniform | Gaussian | Sorted | RevSorted | PipeOrgan | FewVals |
|---|---------|----------|--------|-----------|-----------|---------|
| 1,000 | **fastest** | 2nd | 4th | 3rd | **fastest** | 3rd |
| 10,000 | **fastest** | **fastest** | 3rd | 3rd | **fastest** | 3rd |
| 100,000 | 2nd | 2nd | 2nd | 2nd | **fastest** | 2nd |
| 1,000,000 | 2nd | 2nd | 3rd | 2nd | 2nd | 3rd |
| 5,000,000 | 2nd | 2nd | 3rd | 3rd | 2nd | 3rd |

Canon sort is strongest in the 1k–100k range across most distributions. At 5M elements, IPS4o's more aggressive parallelism pulls ahead. At small sizes, pdqsort wins on already-structured inputs (sorted, few values) due to its pattern detection.

## Requirements

- Intel TBB (`tbb::task_group`)
- A compiler supporting `__asm__` symbol aliasing (GCC, Clang)

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

Supported targets: `x86_64-linux-gnu`, `x86_64-linux-musl`, `aarch64-linux-gnu`, `aarch64-linux-musl`.

Each target produces two files in `dist/<target>/`:
- `libcanon.a` — static archive with TBB merged in (self-contained)
- `libcanon.so` — shared library

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

## Notes

- NaN behaviour is undefined for `f32`/`f64`
- Thread parallelism is bounded by `log2(hardware_concurrency × 8)` depth and only activates for buckets with ≥ 16,384 elements
