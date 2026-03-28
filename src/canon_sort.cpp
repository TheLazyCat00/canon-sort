// SPDX-License-Identifier: MIT
#include "../include/canon_sort.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <thread>
#include <tbb/task_group.h>

/* ------------------------------------------------------------------ */
/* Tunables                                                             */
/* ------------------------------------------------------------------ */

static constexpr int CANON_INSERTION_THRESHOLD = 48;
static constexpr int CANON_BUCKETS             = 2048;

static const int CORES       = std::thread::hardware_concurrency();
static const int IDEAL_DEPTH = (int)std::log2(CORES * 8);

/* ------------------------------------------------------------------ */
/* Type traits                                                          */
/* ------------------------------------------------------------------ */

template <typename T> struct SortTraits;

template <> struct SortTraits<int8_t>   { using U = uint8_t;
	static U ordered(int8_t   v) { return (uint8_t)v;  } };
template <> struct SortTraits<int16_t>  { using U = uint16_t;
	static U ordered(int16_t  v) { return (uint16_t)v; } };
template <> struct SortTraits<int32_t>  { using U = uint32_t;
	static U ordered(int32_t  v) { return (uint32_t)v; } };
template <> struct SortTraits<int64_t>  { using U = uint64_t;
	static U ordered(int64_t  v) { return (uint64_t)v; } };
template <> struct SortTraits<uint8_t>  { using U = uint8_t;
	static U ordered(uint8_t  v) { return v; } };
template <> struct SortTraits<uint16_t> { using U = uint16_t;
	static U ordered(uint16_t v) { return v; } };
template <> struct SortTraits<uint32_t> { using U = uint32_t;
	static U ordered(uint32_t v) { return v; } };
template <> struct SortTraits<uint64_t> { using U = uint64_t;
	static U ordered(uint64_t v) { return v; } };

template <> struct SortTraits<float> {
	using U = uint32_t;
	static U ordered(float v) {
		U u; memcpy(&u, &v, sizeof u);
		U mask = (U)(-(int32_t)(u >> 31)) | 0x80000000u;
		return u ^ mask;
	}
};

template <> struct SortTraits<double> {
	using U = uint64_t;
	static U ordered(double v) {
		U u; memcpy(&u, &v, sizeof u);
		U mask = (U)(-(int64_t)(u >> 63)) | 0x8000000000000000ull;
		return u ^ mask;
	}
};

/* ------------------------------------------------------------------ */
/* Insertion sort                                                       */
/* ------------------------------------------------------------------ */

template <typename T>
static void insertion_sort_slice(T *arr, int low, int high) {
	for (int i = low + 1; i <= high; i++) {
		T key = arr[i];
		int j = i - 1;
		while (j >= low && arr[j] > key) { arr[j+1] = arr[j]; j--; }
		arr[j+1] = key;
	}
}

/* ------------------------------------------------------------------ */
/* Radix sort over a sub-range (two-pass, 11-bit buckets)              */
/* ------------------------------------------------------------------ */

template <typename T>
static void radix_sort_range(T *arr, int low, int high, T *scratch) {
	using U   = typename SortTraits<T>::U;
	using Tr  = SortTraits<T>;

	int  n   = high - low + 1;
	T   *src = arr     + low;
	T   *dst = scratch + low;

	T mn = src[0];
	for (int i = 1; i < n; i++) if (src[i] < mn) mn = src[i];
	U ordered_mn = Tr::ordered(mn);

	{
		int c[2048] = {};
		for (int i = 0; i < n; i++) c[(Tr::ordered(src[i]) - ordered_mn) & 0x7FF]++;
		for (int i = 1; i < 2048; i++) c[i] += c[i-1];
		for (int i = n-1; i >= 0; i--) dst[--c[(Tr::ordered(src[i]) - ordered_mn) & 0x7FF]] = src[i];
	}
	{
		int c[2048] = {};
		for (int i = 0; i < n; i++) c[((Tr::ordered(dst[i]) - ordered_mn) >> 11) & 0x7FF]++;
		for (int i = 1; i < 2048; i++) c[i] += c[i-1];
		for (int i = n-1; i >= 0; i--) src[--c[((Tr::ordered(dst[i]) - ordered_mn) >> 11) & 0x7FF]] = dst[i];
	}
}

/* ------------------------------------------------------------------ */
/* Core recursive routine                                               */
/* ------------------------------------------------------------------ */

template <typename T>
static void canon_rec(T *arr, int low, int high, T *scratch, int depth, T *orig_arr) {
	using U  = typename SortTraits<T>::U;
	using Tr = SortTraits<T>;

	int len = high - low + 1;

	auto leaf_return = [&]() {
		if (arr != orig_arr)
			memcpy(orig_arr + low, arr + low, len * sizeof(T));
	};

	if (len < CANON_INSERTION_THRESHOLD) {
		insertion_sort_slice(arr, low, high);
		leaf_return();
		return;
	}

	/* ---- single-pass range + sortedness scan ---- */
	T   cur_min  = arr[low];
	T   cur_max  = arr[low];
	int inv      = 0;
	int all_desc = 1;

	for (int i = low + 1; i <= high; i++) {
		T x = arr[i], p = arr[i-1];
		if (x < cur_min) cur_min = x;
		if (x > cur_max) cur_max = x;
		if (x > p) { all_desc = 0; if (inv) inv = 2; }
		if (x < p) { inv++;        if (!all_desc) inv = 2; }
		if (inv >= 2 && !all_desc) break;
	}

	if (inv >= 2 && !all_desc) {
		for (int i = low + 1; i <= high; i++) {
			if (arr[i] < cur_min) cur_min = arr[i];
			if (arr[i] > cur_max) cur_max = arr[i];
		}
	}

	/* ---- trivial cases ---- */
	if (cur_min == cur_max) { leaf_return(); return; }
	if (inv == 0)           { leaf_return(); return; }
	if (all_desc) {
		for (int i = low, j = high; i < j; i++, j--)
		{ T t = arr[i]; arr[i] = arr[j]; arr[j] = t; }
		leaf_return();
		return;
	}

	/* ---- bucket count + magic multiplier ---- */
	U ordered_min = Tr::ordered(cur_min);
	U range       = Tr::ordered(cur_max) - ordered_min;

	int num_buckets;
	if      (len <  256)  num_buckets = 16;
	else if (len < 4096)  num_buckets = 64;
	else                  num_buckets = CANON_BUCKETS;

	uint64_t magic = ((uint64_t)(num_buckets - 1) << 32) / (uint64_t)range;

	auto bidx = [&](T v) -> int {
		return (int)((uint64_t)(Tr::ordered(v) - ordered_min) * magic >> 32);
	};

	/* ---- count (4-way unrolled) ---- */
	int c0[CANON_BUCKETS] = {};
	int c1[CANON_BUCKETS] = {};
	int c2[CANON_BUCKETS] = {};
	int c3[CANON_BUCKETS] = {};

	int i = low;
	for (; i <= high - 3; i += 4) {
		c0[bidx(arr[i  ])]++;
		c1[bidx(arr[i+1])]++;
		c2[bidx(arr[i+2])]++;
		c3[bidx(arr[i+3])]++;
	}
	for (; i <= high; i++) c0[bidx(arr[i])]++;
	for (int j = 0; j < num_buckets; j++) c0[j] += c1[j] + c2[j] + c3[j];

	/* ---- degenerate: very few non-empty buckets → radix fallback ---- */
	{
		int nonempty = 0;
		for (int j = 0; j < num_buckets; j++) nonempty += (c0[j] > 0);
		if (nonempty <= 16 && len > 128) {
			radix_sort_range(arr, low, high, scratch);
			leaf_return();
			return;
		}
	}

	/* ---- scatter into scratch ---- */
	int woff[CANON_BUCKETS];
	{
		int pos = low;
		for (int j = 0; j < num_buckets; j++) { woff[j] = pos; pos += c0[j]; }
	}

	i = low;
	for (; i <= high - 3; i += 4) {
		T v0 = arr[i], v1 = arr[i+1], v2 = arr[i+2], v3 = arr[i+3];
		scratch[woff[bidx(v0)]++] = v0;
		scratch[woff[bidx(v1)]++] = v1;
		scratch[woff[bidx(v2)]++] = v2;
		scratch[woff[bidx(v3)]++] = v3;
	}
	for (; i <= high; i++) { T v = arr[i]; scratch[woff[bidx(v)]++] = v; }

	/* ---- recurse (parallel above threshold, swap arr/scratch) ---- */
	tbb::task_group tg;

	int b = low;
	for (int j = 0; j < num_buckets; j++) {
		int sz = c0[j];
		if (sz > 1) {
			int slo = b, shi = b + sz - 1;
			int next_depth = depth > 0 ? depth - 1 : 0;
			if (depth > 0 && sz >= 16384) {
				tg.run([=]() {
					canon_rec<T>(scratch, slo, shi, arr, next_depth, orig_arr);
				});
			} else {
				canon_rec<T>(scratch, slo, shi, arr, next_depth, orig_arr);
			}
		} else if (sz == 1 && scratch != orig_arr) {
			orig_arr[b] = scratch[b];
		}
		b += sz;
	}

	tg.wait();
}

/* ------------------------------------------------------------------ */
/* Typed entry point — allocs scratch, calls canon_rec, frees          */
/* ------------------------------------------------------------------ */

template <typename T>
static void canon_sort_typed(void *ptr, int n) {
	auto *arr     = static_cast<T *>(ptr);
	auto *scratch = static_cast<T *>(malloc(n * sizeof(T)));
	if (!scratch) return;
	canon_rec(arr, 0, n - 1, scratch, IDEAL_DEPTH, arr);
	free(scratch);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void canon_sort_i8 (void *ptr, int n) { canon_sort_typed<int8_t>  (ptr, n); }
void canon_sort_i16(void *ptr, int n) { canon_sort_typed<int16_t> (ptr, n); }
void canon_sort_i32(void *ptr, int n) { canon_sort_typed<int32_t> (ptr, n); }
void canon_sort_i64(void *ptr, int n) { canon_sort_typed<int64_t> (ptr, n); }
void canon_sort_u8 (void *ptr, int n) { canon_sort_typed<uint8_t> (ptr, n); }
void canon_sort_u16(void *ptr, int n) { canon_sort_typed<uint16_t>(ptr, n); }
void canon_sort_u32(void *ptr, int n) { canon_sort_typed<uint32_t>(ptr, n); }
void canon_sort_u64(void *ptr, int n) { canon_sort_typed<uint64_t>(ptr, n); }
void canon_sort_f32(void *ptr, int n) { canon_sort_typed<float>   (ptr, n); }
void canon_sort_f64(void *ptr, int n) { canon_sort_typed<double>  (ptr, n); }

void canon_sort(void *ptr, int n, int type_size) {
	switch (type_size) {
		case CANON_U8:  canon_sort_u8 (ptr, n); break;
		case CANON_U16: canon_sort_u16(ptr, n); break;
		case CANON_U32: canon_sort_u32(ptr, n); break;
		case CANON_U64: canon_sort_u64(ptr, n); break;
		default:
			fprintf(stderr, "canon_sort: unknown type size %d\n", type_size);
			break;
	}
}
