# Canon sort — Technical Specification

## Overview

Canon sort is a recursive, adaptive hybrid sorting algorithm for fixed-width numeric arrays. It partitions input into buckets using a fixed-point range mapping, recurses on each bucket (optionally in parallel), and applies specialised fallbacks for degenerate cases. A single scratch buffer ping-pongs with the source array at each recursion level to avoid per-level allocation and minimise copying.

---

## Algorithm

### Entry Point

`canon_sort_typed<T>(void *ptr, int n)` is the concrete entry for each type `T`. It:

1. Casts `ptr` to `T*`
2. Allocates a scratch buffer of `n × sizeof(T)` bytes
3. Calls `canon_rec(arr, 0, n-1, scratch, IDEAL_DEPTH, arr)`
4. Frees scratch

`IDEAL_DEPTH` is computed once at startup as `floor(log2(hardware_concurrency × 8))`.

---

### Recursive Core: `canon_rec<T>`

Called with `(arr, low, high, scratch, depth, orig_arr)` where `arr` is the current source, `scratch` is the write target, and `orig_arr` is the original caller-owned buffer.

#### Step 1 — Base case: insertion sort

If `high - low + 1 < CANON_INSERTION_THRESHOLD` (48), run insertion sort on `arr[low..high]` in place, then copy the result into `orig_arr[low..high]` if `arr ≠ orig_arr`. Return.

#### Step 2 — Single-pass scan

Scan `arr[low..high]` once to compute:

- `cur_min`, `cur_max` — range bounds
- `inv` — inversion count (clamped at 2, meaning "at least 2 inversions")
- `all_desc` — true if the slice is strictly non-increasing

Early-exit rules applied after the scan:

| Condition | Action |
|-----------|--------|
| `cur_min == cur_max` | All elements equal — copy to `orig_arr` and return |
| `inv == 0` | Already sorted — copy to `orig_arr` and return |
| `all_desc == true` | Reverse `arr[low..high]` in place, copy to `orig_arr` and return |

If the scan aborted early (more than one inversion type detected), a second pass completes the `cur_min`/`cur_max` computation.

#### Step 3 — Value mapping

`SortTraits<T>::ordered(v)` maps each value to an unsigned integer `U` that preserves sort order:

- **Unsigned integers**: identity
- **Signed integers**: reinterpret cast to unsigned (two's complement order matches sort order after adjustment at the bucket level via offset)
- **Floats**: IEEE 754 bit pattern XOR'd with a mask derived from the sign bit, producing a total unsigned order consistent with `<` on non-NaN values

#### Step 4 — Bucket count selection

| Slice length | Bucket count |
|---|---|
| < 256 | 16 |
| < 4096 | 64 |
| ≥ 4096 | 2048 (`CANON_BUCKETS`) |

#### Step 5 — Fixed-point bucket mapping

A magic multiplier is computed once per call:

```
magic = ((num_buckets - 1) << 32) / (ordered(cur_max) - ordered(cur_min))
```

Bucket index for value `v`:

```
bidx(v) = (ordered(v) - ordered(cur_min)) * magic >> 32
```

This maps the value range `[cur_min, cur_max]` onto `[0, num_buckets - 1]` using integer arithmetic with no division per element.

#### Step 6 — Counting (4-way unrolled)

Four count arrays `c0`–`c3` (each of length `num_buckets`) are zeroed and filled with a 4-wide unrolled loop. They are merged back into `c0`:

```
c0[j] += c1[j] + c2[j] + c3[j]  for each j
```

#### Step 7 — Degeneracy check: radix fallback

Count the number of non-empty buckets. If `nonempty ≤ 16` and `len > 128`, fall back to `radix_sort_range` (see below) and return.

This handles highly clustered distributions where bucket partitioning would produce very few, very large buckets and recurse inefficiently.

#### Step 8 — Prefix sum and scatter

Compute write offsets `woff[j]` from `c0` via prefix sum starting at `low`. Scatter `arr[low..high]` into `scratch[low..high]` using a 4-wide unrolled loop, incrementing `woff[j]` as each element is placed.

#### Step 9 — Parallel or serial recursion

For each bucket `j` with size `sz`:

- If `sz == 1` and `scratch ≠ orig_arr`: copy the single element directly to `orig_arr`
- If `sz > 1` and `depth > 0` and `sz ≥ 16384`: spawn `canon_rec(scratch, slo, shi, arr, depth-1, orig_arr)` as a TBB task
- Otherwise: call `canon_rec(scratch, slo, shi, arr, depth-1, orig_arr)` serially

Note the buffer swap: `scratch` becomes the new source and `arr` becomes the new scratch target. This alternation means data ping-pongs between the two buffers at each level with no additional allocation.

After spawning all parallel tasks, call `tg.wait()`.

---

### Radix Sort Fallback: `radix_sort_range<T>`

A two-pass LSD radix sort over `arr[low..high]` using 11-bit buckets (2048 bins per pass), covering 22 bits of the value range in two passes.

Each pass:
1. Subtracts `ordered(min)` from each value before extracting bits, confining the range to `[0, range]`
2. Counts into a 2048-element histogram
3. Converts to prefix sums
4. Scatters backwards (stable)

Pass 1 uses bits 0–10; pass 2 uses bits 11–21. Source and scratch swap between passes.

---

## Buffer Management

One scratch buffer is allocated at the top level. At each recursion level, source and scratch swap roles. Leaves copy back to `orig_arr` only if their current source buffer is not `orig_arr`. On average, half of all leaves skip this copy.

No heap allocation occurs below the top-level call.

---

## Parallelism

Parallelism is provided by `tbb::task_group` with TBB's work-stealing scheduler. Parallel task spawning is gated on two conditions:

- `depth > 0` (within the parallel depth budget)
- `sz ≥ 16384` (bucket is large enough to justify thread overhead)

`IDEAL_DEPTH` caps the total parallel depth at `floor(log2(hardware_concurrency × 8))`, preventing excessive task proliferation on large inputs.

---

## Constants

| Name | Value | Purpose |
|---|---|---|
| `CANON_INSERTION_THRESHOLD` | 48 | Slice size below which insertion sort is used |
| `CANON_BUCKETS` | 2048 | Maximum bucket count for large slices |
| `IDEAL_DEPTH` | `floor(log2(cores × 8))` | Maximum parallel recursion depth |
| Radix fallback threshold | 16 non-empty buckets, len > 128 | Triggers 2-pass radix sort |
| Parallel task threshold | 16384 elements | Minimum bucket size to spawn a TBB task |

---

## Type Support

| Tag | C type | Ordered mapping |
|---|---|---|
| `CANON_U8` | `uint8_t` | Identity |
| `CANON_U16` | `uint16_t` | Identity |
| `CANON_U32` | `uint32_t` | Identity |
| `CANON_U64` | `uint64_t` | Identity |
| — | `int8_t` | Reinterpret as `uint8_t` |
| — | `int16_t` | Reinterpret as `uint16_t` |
| — | `int32_t` | Reinterpret as `uint32_t` |
| — | `int64_t` | Reinterpret as `uint64_t` |
| — | `float` | XOR with sign-derived mask |
| — | `double` | XOR with sign-derived mask |

Signed integer ordering works because the offset subtraction (`ordered(v) - ordered(cur_min)`) cancels the two's complement bias, producing a correct relative unsigned order within the slice.

---

## Complexity

| Case | Time | Space |
|---|---|---|
| Average | O(n) | O(n) |
| Already sorted | O(n) | O(n) |
| Already reversed | O(n) | O(n) |
| All equal | O(n) | O(n) |
| Worst case | O(n · log_k(n)), k = bucket count | O(n) |

Space is O(n) for the scratch buffer; stack depth is bounded by `IDEAL_DEPTH` for parallel paths and by `log(n / CANON_INSERTION_THRESHOLD)` for serial paths.

---

## Limitations

- NaN behaviour is undefined for `f32` and `f64`
- The generic `canon_sort` dispatcher supports only unsigned integer type tags (`CANON_U8`–`CANON_U64`); signed and floating-point types must use typed entry points
- Requires Intel TBB; not compatible with environments without TBB
- Element count is `int`; arrays larger than `INT_MAX` elements are not supported
