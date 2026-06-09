// SPDX-License-Identifier: MIT
#include "../include/canon_sort.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <thread>
#include <algorithm>
#include <type_traits>
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#include <tbb/task_group.h>

/* ------------------------------------------------------------------ */
/* Tunables                                                             */
/* ------------------------------------------------------------------ */

static constexpr int CANON_INSERTION_THRESHOLD = 48;
static constexpr int CANON_BUCKETS             = 2048;

static const int CORES       = std::thread::hardware_concurrency() ? (int)std::thread::hardware_concurrency() : 1;
static const int IDEAL_DEPTH = (int)std::log2(CORES * 8);

/* ------------------------------------------------------------------ */
/* Type traits                                                          */
/* ------------------------------------------------------------------ */

template <typename T> struct SortTraits;

template <> struct SortTraits<int8_t>   { using U = uint8_t;
	static U ordered(int8_t   v) { return (uint8_t)v  ^ 0x80u; } };
template <> struct SortTraits<int16_t>  { using U = uint16_t;
	static U ordered(int16_t  v) { return (uint16_t)v ^ 0x8000u; } };
template <> struct SortTraits<int32_t>  { using U = uint32_t;
	static U ordered(int32_t  v) { return (uint32_t)v ^ 0x80000000u; } };
template <> struct SortTraits<int64_t>  { using U = uint64_t;
	static U ordered(int64_t  v) { return (uint64_t)v ^ 0x8000000000000000ull; } };
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
		if (key >= arr[i-1]) continue;
		int j = i - 1;
		do { arr[j+1] = arr[j]; j--; } while (j >= low && arr[j] > key);
		arr[j+1] = key;
	}
}

/* ------------------------------------------------------------------ */
/* Radix sort over a sub-range (variable passes, 11-bit buckets)       */
/* ------------------------------------------------------------------ */

template <typename U>
static inline int used_bits(U x) {
	if (!x) return 0;
	if constexpr (sizeof(U) <= 4) {
		return 32 - __builtin_clz((uint32_t)x);
	} else {
		return 64 - __builtin_clzll((uint64_t)x);
	}
}

template <typename U>
struct RangeMapper {
	using Magic = std::conditional_t<(sizeof(U) <= 4), uint64_t, unsigned __int128>;

	U ordered_min;
	Magic magic;

	RangeMapper(U min_value, U range, int num_buckets) : ordered_min(min_value) {
		if constexpr (sizeof(U) <= 4) {
			magic = ((uint64_t)(num_buckets - 1) << 32) / (uint64_t)range;
		} else {
			magic = ((unsigned __int128)(num_buckets - 1) << 64) / (unsigned __int128)range;
		}
	}

	inline int index(U ordered) const {
		U delta = ordered - ordered_min;
		if constexpr (sizeof(U) <= 4) {
			return (int)(((uint64_t)delta * magic) >> 32);
		} else {
			return (int)(((unsigned __int128)delta * magic) >> 64);
		}
	}
};

template <typename T>
static void radix_sort_range(T *arr, int low, int high, T *scratch) {
	using U  = typename SortTraits<T>::U;
	using Tr = SortTraits<T>;

	int n = high - low + 1;
	T *src = arr + low;
	T *dst = scratch + low;

	T mn = src[0];
	T mx = src[0];
	for (int i = 1; i < n; i++) {
		if (src[i] < mn) mn = src[i];
		if (src[i] > mx) mx = src[i];
	}

	U ordered_mn = Tr::ordered(mn);
	U range = Tr::ordered(mx) - ordered_mn;
	int bits = used_bits(range);
	if (bits == 0) return;
	int passes = (bits + 10) / 11;

	for (int pass = 0; pass < passes; pass++) {
		int shift = pass * 11;
		int chunk_bits = bits - shift;
		if (chunk_bits > 11) chunk_bits = 11;
		int bucket_count = 1 << chunk_bits;
		U mask = (U)(bucket_count - 1);

		alignas(64) int c[2048];
		std::fill_n(c, bucket_count, 0);

		for (int i = 0; i < n; i++) c[((Tr::ordered(src[i]) - ordered_mn) >> shift) & mask]++;
		for (int i = 1; i < bucket_count; i++) c[i] += c[i - 1];
		for (int i = n - 1; i >= 0; i--) {
			U key = (Tr::ordered(src[i]) - ordered_mn) >> shift;
			dst[--c[key & mask]] = src[i];
		}
		T *tmp = src; src = dst; dst = tmp;
	}

	if (src != arr + low) memcpy(arr + low, src, (size_t)n * sizeof(T));
}

#if defined(__AVX2__)
static inline void reverse_u32_slice_avx2(uint32_t *arr, int low, int high) {
	const __m256i rev_idx = _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0);
	while (high - low >= 15) {
		__m256i lo = _mm256_loadu_si256((const __m256i *)(arr + low));
		__m256i hi = _mm256_loadu_si256((const __m256i *)(arr + high - 7));
		lo = _mm256_permutevar8x32_epi32(lo, rev_idx);
		hi = _mm256_permutevar8x32_epi32(hi, rev_idx);
		_mm256_storeu_si256((__m256i *)(arr + low), hi);
		_mm256_storeu_si256((__m256i *)(arr + high - 7), lo);
		low += 8;
		high -= 8;
	}
	while (low < high) {
		uint32_t t = arr[low];
		arr[low] = arr[high];
		arr[high] = t;
		low++;
		high--;
	}
}
#endif

template <typename T>
static inline void reverse_slice(T *arr, int low, int high) {
#if defined(__AVX2__)
	if constexpr (std::is_same_v<T, uint32_t>) {
		reverse_u32_slice_avx2(arr, low, high);
		return;
	}
#endif
	while (low < high) {
		T t = arr[low];
		arr[low] = arr[high];
		arr[high] = t;
		low++;
		high--;
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
		reverse_slice(arr, low, high);
		leaf_return();
		return;
	}

	/* ---- bucket count + magic multiplier ---- */
	U ordered_min = Tr::ordered(cur_min);
	U range       = Tr::ordered(cur_max) - ordered_min;
	int range_bits = used_bits(range);
	int radix_passes = (range_bits + 10) / 11;
	if (len >= 4096 && radix_passes <= 2) {
		int sample_points = len - 1;
		if (sample_points > 255) sample_points = 255;
		int flips = 0;
		int nonzero = 0;
		int prev_sign = 0;
		for (int k = 1; k <= sample_points; k++) {
			int idx = low + k;
			int sign = (arr[idx] > arr[idx - 1]) - (arr[idx] < arr[idx - 1]);
			if (sign) {
				if (prev_sign && sign != prev_sign) flips++;
				prev_sign = sign;
				nonzero++;
			}
		}
		bool zigzag = nonzero >= 16 && flips * 10 >= (nonzero - 1) * 8;
		if (!zigzag) {
			radix_sort_range(arr, low, high, scratch);
			leaf_return();
			return;
		}
	}

	int num_buckets;
	if      (len <  256)  num_buckets = 16;
	else if (len < 4096)  num_buckets = 64;
	else                  num_buckets = CANON_BUCKETS;

	RangeMapper<U> mapper(ordered_min, range, num_buckets);

	auto bidx = [&](T v) -> int {
		return mapper.index(Tr::ordered(v));
	};

	/* ---- count (4-way unrolled) ---- */
	alignas(64) int c0[CANON_BUCKETS];
	alignas(64) int c1[CANON_BUCKETS];
	alignas(64) int c2[CANON_BUCKETS];
	alignas(64) int c3[CANON_BUCKETS];
	std::fill_n(c0, num_buckets, 0);
	std::fill_n(c1, num_buckets, 0);
	std::fill_n(c2, num_buckets, 0);
	std::fill_n(c3, num_buckets, 0);

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
	int b = low;
	if (depth > 0) {
		tbb::task_group tg;
		bool spawned = false;
		for (int j = 0; j < num_buckets; j++) {
			int sz = c0[j];
			if (sz > 1) {
				int slo = b, shi = b + sz - 1;
				int next_depth = depth - 1;
				if (sz >= 16384) {
					spawned = true;
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
		if (spawned) tg.wait();
	} else {
		for (int j = 0; j < num_buckets; j++) {
			int sz = c0[j];
			if (sz > 1) {
				canon_rec<T>(scratch, b, b + sz - 1, arr, 0, orig_arr);
			} else if (sz == 1 && scratch != orig_arr) {
				orig_arr[b] = scratch[b];
			}
			b += sz;
		}
	}
}


/* ------------------------------------------------------------------ */
/* Indirect pointer sorting by cached numeric keys                      */
/* ------------------------------------------------------------------ */

template <typename K>
static void insertion_sort_ptrs_slice(void **ptrs, K *keys, int low, int high) {
	for (int i = low + 1; i <= high; i++) {
		K key = keys[i];
		void *ptr = ptrs[i];
		if (key >= keys[i-1]) continue;
		int j = i - 1;
		do {
			keys[j+1] = keys[j];
			ptrs[j+1] = ptrs[j];
			j--;
		} while (j >= low && keys[j] > key);
		keys[j+1] = key;
		ptrs[j+1] = ptr;
	}
}

template <typename K>
static void reverse_ptrs_keys(void **ptrs, K *keys, int low, int high) {
	while (low < high) {
		K k = keys[low];
		keys[low] = keys[high];
		keys[high] = k;
		void *p = ptrs[low];
		ptrs[low] = ptrs[high];
		ptrs[high] = p;
		low++;
		high--;
	}
}

template <typename K>
static void radix_sort_ptrs_range(void **ptrs, K *keys, int low, int high,
		void **ptr_scratch, K *key_scratch) {
	using U  = typename SortTraits<K>::U;
	using Tr = SortTraits<K>;

	int n = high - low + 1;
	void **src_ptrs = ptrs + low;
	void **dst_ptrs = ptr_scratch + low;
	K *src_keys = keys + low;
	K *dst_keys = key_scratch + low;

	K mn = src_keys[0];
	K mx = src_keys[0];
	for (int i = 1; i < n; i++) {
		if (src_keys[i] < mn) mn = src_keys[i];
		if (src_keys[i] > mx) mx = src_keys[i];
	}

	U ordered_mn = Tr::ordered(mn);
	U range = Tr::ordered(mx) - ordered_mn;
	int bits = used_bits(range);
	if (bits == 0) return;
	int passes = (bits + 10) / 11;

	for (int pass = 0; pass < passes; pass++) {
		int shift = pass * 11;
		int chunk_bits = bits - shift;
		if (chunk_bits > 11) chunk_bits = 11;
		int bucket_count = 1 << chunk_bits;
		U mask = (U)(bucket_count - 1);

		alignas(64) int c[2048];
		std::fill_n(c, bucket_count, 0);

		for (int i = 0; i < n; i++) c[((Tr::ordered(src_keys[i]) - ordered_mn) >> shift) & mask]++;
		for (int i = 1; i < bucket_count; i++) c[i] += c[i - 1];
		for (int i = n - 1; i >= 0; i--) {
			U key = (Tr::ordered(src_keys[i]) - ordered_mn) >> shift;
			int pos = --c[key & mask];
			dst_keys[pos] = src_keys[i];
			dst_ptrs[pos] = src_ptrs[i];
		}
		K *tmp_keys = src_keys; src_keys = dst_keys; dst_keys = tmp_keys;
		void **tmp_ptrs = src_ptrs; src_ptrs = dst_ptrs; dst_ptrs = tmp_ptrs;
	}

	if (src_keys != keys + low) {
		memcpy(keys + low, src_keys, (size_t)n * sizeof(K));
		memcpy(ptrs + low, src_ptrs, (size_t)n * sizeof(void *));
	}
}

template <typename K>
static void canon_rec_ptrs(void **ptrs, K *keys, int low, int high,
		void **ptr_scratch, K *key_scratch, int depth,
		void **orig_ptrs, K *orig_keys) {
	using U  = typename SortTraits<K>::U;
	using Tr = SortTraits<K>;

	int len = high - low + 1;

	auto leaf_return = [&]() {
		if (ptrs != orig_ptrs) {
			memcpy(orig_ptrs + low, ptrs + low, (size_t)len * sizeof(void *));
			memcpy(orig_keys + low, keys + low, (size_t)len * sizeof(K));
		}
	};

	if (len < CANON_INSERTION_THRESHOLD) {
		insertion_sort_ptrs_slice(ptrs, keys, low, high);
		leaf_return();
		return;
	}

	K cur_min = keys[low];
	K cur_max = keys[low];
	int inv = 0;
	int all_desc = 1;

	for (int i = low + 1; i <= high; i++) {
		K x = keys[i], p = keys[i-1];
		if (x < cur_min) cur_min = x;
		if (x > cur_max) cur_max = x;
		if (x > p) { all_desc = 0; if (inv) inv = 2; }
		if (x < p) { inv++;        if (!all_desc) inv = 2; }
		if (inv >= 2 && !all_desc) break;
	}

	if (inv >= 2 && !all_desc) {
		for (int i = low + 1; i <= high; i++) {
			if (keys[i] < cur_min) cur_min = keys[i];
			if (keys[i] > cur_max) cur_max = keys[i];
		}
	}

	if (cur_min == cur_max) { leaf_return(); return; }
	if (inv == 0)           { leaf_return(); return; }
	if (all_desc) {
		reverse_ptrs_keys(ptrs, keys, low, high);
		leaf_return();
		return;
	}

	U ordered_min = Tr::ordered(cur_min);
	U range       = Tr::ordered(cur_max) - ordered_min;
	int range_bits = used_bits(range);
	int radix_passes = (range_bits + 10) / 11;
	if (len >= 4096 && radix_passes <= 2) {
		int sample_points = len - 1;
		if (sample_points > 255) sample_points = 255;
		int flips = 0;
		int nonzero = 0;
		int prev_sign = 0;
		for (int k = 1; k <= sample_points; k++) {
			int idx = low + k;
			int sign = (keys[idx] > keys[idx - 1]) - (keys[idx] < keys[idx - 1]);
			if (sign) {
				if (prev_sign && sign != prev_sign) flips++;
				prev_sign = sign;
				nonzero++;
			}
		}
		bool zigzag = nonzero >= 16 && flips * 10 >= (nonzero - 1) * 8;
		if (!zigzag) {
			radix_sort_ptrs_range(ptrs, keys, low, high, ptr_scratch, key_scratch);
			leaf_return();
			return;
		}
	}

	int num_buckets;
	if      (len <  256)  num_buckets = 16;
	else if (len < 4096)  num_buckets = 64;
	else                  num_buckets = CANON_BUCKETS;

	RangeMapper<U> mapper(ordered_min, range, num_buckets);

	auto bidx = [&](K v) -> int {
		return mapper.index(Tr::ordered(v));
	};

	alignas(64) int c0[CANON_BUCKETS];
	alignas(64) int c1[CANON_BUCKETS];
	alignas(64) int c2[CANON_BUCKETS];
	alignas(64) int c3[CANON_BUCKETS];
	std::fill_n(c0, num_buckets, 0);
	std::fill_n(c1, num_buckets, 0);
	std::fill_n(c2, num_buckets, 0);
	std::fill_n(c3, num_buckets, 0);

	int i = low;
	for (; i <= high - 3; i += 4) {
		c0[bidx(keys[i  ])]++;
		c1[bidx(keys[i+1])]++;
		c2[bidx(keys[i+2])]++;
		c3[bidx(keys[i+3])]++;
	}
	for (; i <= high; i++) c0[bidx(keys[i])]++;
	for (int j = 0; j < num_buckets; j++) c0[j] += c1[j] + c2[j] + c3[j];

	{
		int nonempty = 0;
		for (int j = 0; j < num_buckets; j++) nonempty += (c0[j] > 0);
		if (nonempty <= 16 && len > 128) {
			radix_sort_ptrs_range(ptrs, keys, low, high, ptr_scratch, key_scratch);
			leaf_return();
			return;
		}
	}

	int woff[CANON_BUCKETS];
	{
		int pos = low;
		for (int j = 0; j < num_buckets; j++) { woff[j] = pos; pos += c0[j]; }
	}

	i = low;
	for (; i <= high - 3; i += 4) {
		K k0 = keys[i], k1 = keys[i+1], k2 = keys[i+2], k3 = keys[i+3];
		void *p0 = ptrs[i], *p1 = ptrs[i+1], *p2 = ptrs[i+2], *p3 = ptrs[i+3];
		int b0 = bidx(k0), b1 = bidx(k1), b2 = bidx(k2), b3 = bidx(k3);
		key_scratch[woff[b0]] = k0; ptr_scratch[woff[b0]++] = p0;
		key_scratch[woff[b1]] = k1; ptr_scratch[woff[b1]++] = p1;
		key_scratch[woff[b2]] = k2; ptr_scratch[woff[b2]++] = p2;
		key_scratch[woff[b3]] = k3; ptr_scratch[woff[b3]++] = p3;
	}
	for (; i <= high; i++) {
		K k = keys[i];
		int b = bidx(k);
		key_scratch[woff[b]] = k;
		ptr_scratch[woff[b]++] = ptrs[i];
	}

	int b = low;
	if (depth > 0) {
		tbb::task_group tg;
		bool spawned = false;
		for (int j = 0; j < num_buckets; j++) {
			int sz = c0[j];
			if (sz > 1) {
				int slo = b, shi = b + sz - 1;
				int next_depth = depth - 1;
				if (sz >= 16384) {
					spawned = true;
					tg.run([=]() {
						canon_rec_ptrs<K>(ptr_scratch, key_scratch, slo, shi, ptrs, keys, next_depth, orig_ptrs, orig_keys);
					});
				} else {
					canon_rec_ptrs<K>(ptr_scratch, key_scratch, slo, shi, ptrs, keys, next_depth, orig_ptrs, orig_keys);
				}
			} else if (sz == 1 && ptr_scratch != orig_ptrs) {
				orig_ptrs[b] = ptr_scratch[b];
				orig_keys[b] = key_scratch[b];
			}
			b += sz;
		}
		if (spawned) tg.wait();
	} else {
		for (int j = 0; j < num_buckets; j++) {
			int sz = c0[j];
			if (sz > 1) {
				canon_rec_ptrs<K>(ptr_scratch, key_scratch, b, b + sz - 1, ptrs, keys, 0, orig_ptrs, orig_keys);
			} else if (sz == 1 && ptr_scratch != orig_ptrs) {
				orig_ptrs[b] = ptr_scratch[b];
				orig_keys[b] = key_scratch[b];
			}
			b += sz;
		}
	}
}

template <typename K>
static void canon_sort_ptrs_typed(void *ptrs_void, const K *in_keys, int n) {
	if (n < 2) return;
	auto **ptrs = static_cast<void **>(ptrs_void);
	auto *keys = static_cast<K *>(malloc((size_t)n * sizeof(K)));
	auto **ptr_scratch = static_cast<void **>(malloc((size_t)n * sizeof(void *)));
	auto *key_scratch = static_cast<K *>(malloc((size_t)n * sizeof(K)));
	if (!keys || !ptr_scratch || !key_scratch) {
		free(keys);
		free(ptr_scratch);
		free(key_scratch);
		return;
	}
	memcpy(keys, in_keys, (size_t)n * sizeof(K));
	canon_rec_ptrs<K>(ptrs, keys, 0, n - 1, ptr_scratch, key_scratch, IDEAL_DEPTH, ptrs, keys);
	free(keys);
	free(ptr_scratch);
	free(key_scratch);
}

/* ------------------------------------------------------------------ */
/* Typed entry point — allocs scratch, calls canon_rec, frees          */
/* ------------------------------------------------------------------ */

template <typename T>
static void canon_sort_typed(void *ptr, int n) {
	if (n < 2) return;
	auto *arr     = static_cast<T *>(ptr);
	auto *scratch = static_cast<T *>(malloc((size_t)n * sizeof(T)));
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

void canon_sort_ptrs_i8 (void *ptrs, const int8_t   *keys, int n) { canon_sort_ptrs_typed<int8_t>  (ptrs, keys, n); }
void canon_sort_ptrs_i16(void *ptrs, const int16_t  *keys, int n) { canon_sort_ptrs_typed<int16_t> (ptrs, keys, n); }
void canon_sort_ptrs_i32(void *ptrs, const int32_t  *keys, int n) { canon_sort_ptrs_typed<int32_t> (ptrs, keys, n); }
void canon_sort_ptrs_i64(void *ptrs, const int64_t  *keys, int n) { canon_sort_ptrs_typed<int64_t> (ptrs, keys, n); }
void canon_sort_ptrs_u8 (void *ptrs, const uint8_t  *keys, int n) { canon_sort_ptrs_typed<uint8_t> (ptrs, keys, n); }
void canon_sort_ptrs_u16(void *ptrs, const uint16_t *keys, int n) { canon_sort_ptrs_typed<uint16_t>(ptrs, keys, n); }
void canon_sort_ptrs_u32(void *ptrs, const uint32_t *keys, int n) { canon_sort_ptrs_typed<uint32_t>(ptrs, keys, n); }
void canon_sort_ptrs_u64(void *ptrs, const uint64_t *keys, int n) { canon_sort_ptrs_typed<uint64_t>(ptrs, keys, n); }
void canon_sort_ptrs_f32(void *ptrs, const float    *keys, int n) { canon_sort_ptrs_typed<float>   (ptrs, keys, n); }
void canon_sort_ptrs_f64(void *ptrs, const double   *keys, int n) { canon_sort_ptrs_typed<double>  (ptrs, keys, n); }

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
