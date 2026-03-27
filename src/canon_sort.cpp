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

#define CANON_INSERTION_THRESHOLD 48
#define CANON_BUCKETS             2048

static const int CORES       = std::thread::hardware_concurrency();
static const int IDEAL_DEPTH = (int)std::log2(CORES * 8);

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static inline void swap_int(int *a, int *b) { int t = *a; *a = *b; *b = t; }

/* ------------------------------------------------------------------ */
/* Insertion sort                                                       */
/* ------------------------------------------------------------------ */

static void insertion_sort_slice(int *arr, int low, int high) {
	for (int i = low + 1; i <= high; i++) {
		int key = arr[i], j = i - 1;
		while (j >= low && arr[j] > key) { arr[j+1] = arr[j]; j--; }
		arr[j+1] = key;
	}
}

/* ------------------------------------------------------------------ */
/* Radix sort over a sub-range (two-pass, 11-bit buckets)              */
/* ------------------------------------------------------------------ */

static void radix_sort_range(int *arr, int low, int high, int *scratch) {
	int  n   = high - low + 1;
	int *src = arr     + low;
	int *dst = scratch + low;

	int mn = src[0];
	for (int i = 1; i < n; i++) if (src[i] < mn) mn = src[i];

	{
		int c[2048] = {0};
		for (int i = 0; i < n; i++) c[(src[i] - mn) & 0x7FF]++;
		for (int i = 1; i < 2048; i++) c[i] += c[i-1];
		for (int i = n-1; i >= 0; i--) dst[--c[(src[i] - mn) & 0x7FF]] = src[i];
	}
	{
		int c[2048] = {0};
		for (int i = 0; i < n; i++) c[((dst[i] - mn) >> 11) & 0x7FF]++;
		for (int i = 1; i < 2048; i++) c[i] += c[i-1];
		for (int i = n-1; i >= 0; i--) src[--c[((dst[i] - mn) >> 11) & 0x7FF]] = dst[i];
	}
}

/* ------------------------------------------------------------------ */
/* Core recursive routine                                               */
/* ------------------------------------------------------------------ */

static void canon_rec(int *arr, int low, int high, int *scratch, int depth, int *orig_arr) {
	int len = high - low + 1;

#define LEAF_RETURN() \
	do { \
		if (arr != orig_arr) memcpy(orig_arr + low, arr + low, len * sizeof(int)); \
		return; \
	} while(0)

	if (len < CANON_INSERTION_THRESHOLD) {
		insertion_sort_slice(arr, low, high);
		LEAF_RETURN();
	}

	/* ---- single-pass range + sortedness scan ---- */
	int cur_min  = arr[low];
	int cur_max  = arr[low];
	int inv      = 0;
	int all_desc = 1;

	for (int i = low + 1; i <= high; i++) {
		int x = arr[i], p = arr[i-1];
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
	if (cur_min == cur_max) LEAF_RETURN();
	if (inv == 0)           LEAF_RETURN();
	if (all_desc) {
		for (int i = low, j = high; i < j; i++, j--) swap_int(&arr[i], &arr[j]);
		LEAF_RETURN();
	}

	/* ---- choose bucket count by size ---- */
	long long range = (long long)cur_max - cur_min;

	int num_buckets;
	if      (len <  256)  num_buckets = 16;
	else if (len < 4096)  num_buckets = 64;
	else                  num_buckets = CANON_BUCKETS;

	uint64_t magic = ((uint64_t)(num_buckets - 1) << 32) / (uint64_t)range;

#define BIDX(v) ((uint32_t)(((uint64_t)((v) - cur_min) * magic) >> 32))

	/* ---- count (4-way unrolled) ---- */
	int c0[CANON_BUCKETS] = {0};
	int c1[CANON_BUCKETS] = {0};
	int c2[CANON_BUCKETS] = {0};
	int c3[CANON_BUCKETS] = {0};

	int i = low;
	for (; i <= high - 3; i += 4) {
		c0[BIDX(arr[i  ])]++;
		c1[BIDX(arr[i+1])]++;
		c2[BIDX(arr[i+2])]++;
		c3[BIDX(arr[i+3])]++;
	}
	for (; i <= high; i++) c0[BIDX(arr[i])]++;
	for (int j = 0; j < num_buckets; j++) c0[j] += c1[j] + c2[j] + c3[j];

	/* ---- degenerate: very few non-empty buckets → radix fallback ---- */
	{
		int nonempty = 0;
		for (int j = 0; j < num_buckets; j++) nonempty += (c0[j] > 0);
		if (nonempty <= 16 && len > 128) {
			radix_sort_range(arr, low, high, scratch);
			LEAF_RETURN();
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
		int v0 = arr[i], v1 = arr[i+1], v2 = arr[i+2], v3 = arr[i+3];
		scratch[woff[BIDX(v0)]++] = v0;
		scratch[woff[BIDX(v1)]++] = v1;
		scratch[woff[BIDX(v2)]++] = v2;
		scratch[woff[BIDX(v3)]++] = v3;
	}
	for (; i <= high; i++) { int v = arr[i]; scratch[woff[BIDX(v)]++] = v; }

	#undef BIDX
	#undef LEAF_RETURN

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
					canon_rec(scratch, slo, shi, arr, next_depth, orig_arr);
				});
			} else {
				canon_rec(scratch, slo, shi, arr, next_depth, orig_arr);
			}
		} else if (sz == 1 && scratch != orig_arr) {
			orig_arr[b] = scratch[b];
		}
		b += sz;
	}

	tg.wait();
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                   */
/* ------------------------------------------------------------------ */

void canon_sort_int(int *arr, int n) {
	if (n < 2) return;
	int *scratch = (int*)malloc(n * sizeof(int));
	canon_rec(arr, 0, n - 1, scratch, IDEAL_DEPTH, arr);
	free(scratch);
}
