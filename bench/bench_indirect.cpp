// SPDX-License-Identifier: MIT
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../include/canon_sort.hpp"

struct HeapObject {
	uint32_t key;
	uint32_t salt0;
	uint32_t salt1;
	uint32_t salt2;
	char payload[32];
};

/* ------------------------------------------------------------------ */
/* Timing                                                               */
/* ------------------------------------------------------------------ */

static double now_sec(void) {
	auto t = std::chrono::steady_clock::now().time_since_epoch();
	return std::chrono::duration<double>(t).count();
}

/* ------------------------------------------------------------------ */
/* RNG                                                                  */
/* ------------------------------------------------------------------ */

static uint64_t rng_state;
static void     rng_reset(void) { rng_state = 0xdeadbeefcafe1234ULL; }
static uint32_t rng_next(void) {
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (uint32_t)(rng_state * 0x2545F4914F6CDD1DULL >> 32);
}

/* ------------------------------------------------------------------ */
/* Key functions                                                        */
/* ------------------------------------------------------------------ */

static inline uint32_t cheap_key(const HeapObject &o) {
	return o.key;
}

static inline uint32_t rotl32(uint32_t x, int r) {
	return (x << r) | (x >> (32 - r));
}

static inline uint32_t expensive_key(const HeapObject &o) {
	uint32_t x = o.key ^ rotl32(o.salt0, 5) ^ rotl32(o.salt1, 11) ^ rotl32(o.salt2, 17);
	x ^= (uint32_t)(unsigned char)o.payload[3]  * 0x9E3779B1u;
	x ^= (uint32_t)(unsigned char)o.payload[11] * 0x85EBCA77u;
	x ^= (uint32_t)(unsigned char)o.payload[19] * 0xC2B2AE3Du;
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

/* ------------------------------------------------------------------ */
/* Distributions                                                        */
/* ------------------------------------------------------------------ */

static void fill_object_noise(HeapObject &o) {
	o.salt0 = rng_next();
	o.salt1 = rng_next();
	o.salt2 = rng_next();
	for (int i = 0; i < (int)sizeof(o.payload); i++) o.payload[i] = (char)rng_next();
}

static std::vector<HeapObject> make_uniform(int n, int range) {
	std::vector<HeapObject> a(n);
	for (int i = 0; i < n; i++) { a[i].key = rng_next() % range; fill_object_noise(a[i]); }
	return a;
}

static std::vector<HeapObject> make_gaussian(int n, int range) {
	std::vector<HeapObject> a(n);
	for (int i = 0; i < n; i++) {
		double u1 = (rng_next() + 1.0) / 4294967296.0;
		double u2 =  rng_next()        / 4294967296.0;
		double z  = std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * 3.14159265358979323846 * u2);
		double v  = (z / 6.0 + 0.5) * range;
		if (v < 0) v = 0; else if (v >= range) v = range - 1;
		a[i].key = (uint32_t)v;
		fill_object_noise(a[i]);
	}
	return a;
}

static std::vector<HeapObject> make_few_values(int n) {
	std::vector<HeapObject> a(n);
	for (int i = 0; i < n; i++) { a[i].key = rng_next() % 8; fill_object_noise(a[i]); }
	return a;
}

static std::vector<HeapObject> make_sorted(int n, int range) {
	auto a = make_uniform(n, range);
	std::sort(a.begin(), a.end(), [](const HeapObject &x, const HeapObject &y) { return x.key < y.key; });
	return a;
}

/* ------------------------------------------------------------------ */
/* Correctness                                                          */
/* ------------------------------------------------------------------ */

template <typename KeyFn>
static void verify_objects(const char *name, const HeapObject *arr, int n, KeyFn key_fn) {
	for (int i = 0; i < n - 1; i++) {
		if (key_fn(arr[i]) > key_fn(arr[i+1])) {
			std::fprintf(stderr, "CORRECTNESS FAILURE: %s at [%d]\n", name, i);
			std::exit(1);
		}
	}
}

template <typename KeyFn>
static void verify_ptrs(const char *name, HeapObject *const *arr, int n, KeyFn key_fn) {
	for (int i = 0; i < n - 1; i++) {
		if (key_fn(*arr[i]) > key_fn(*arr[i+1])) {
			std::fprintf(stderr, "CORRECTNESS FAILURE: %s at [%d]\n", name, i);
			std::exit(1);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Sort adapters                                                        */
/* ------------------------------------------------------------------ */

template <typename KeyFn>
static void std_sort_objects(HeapObject *objs, int n, KeyFn key_fn) {
	std::sort(objs, objs + n, [&](const HeapObject &a, const HeapObject &b) {
		return key_fn(a) < key_fn(b);
	});
}

template <typename KeyFn>
static void std_sort_ptrs(HeapObject **ptrs, int n, KeyFn key_fn) {
	std::sort(ptrs, ptrs + n, [&](const HeapObject *a, const HeapObject *b) {
		return key_fn(*a) < key_fn(*b);
	});
}

template <typename KeyFn>
static void canon_sort_ptrs_wrapper(HeapObject **ptrs, int n, KeyFn key_fn) {
	canon_sort_ptrs_by(ptrs, n, key_fn);
}

/* ------------------------------------------------------------------ */
/* Benchmark                                                            */
/* ------------------------------------------------------------------ */

#define WARMUP_REPS  2
#define BENCH_REPS   9

template <typename KeyFn>
static double bench_objects(const char *name, const std::vector<HeapObject> &src, KeyFn key_fn) {
	std::vector<HeapObject> buf(src.size());
	double times[BENCH_REPS];

	for (int r = 0; r < WARMUP_REPS; r++) {
		memcpy(buf.data(), src.data(), src.size() * sizeof(HeapObject));
		std_sort_objects(buf.data(), (int)buf.size(), key_fn);
	}
	for (int r = 0; r < BENCH_REPS; r++) {
		memcpy(buf.data(), src.data(), src.size() * sizeof(HeapObject));
		double t0 = now_sec();
		std_sort_objects(buf.data(), (int)buf.size(), key_fn);
		double t1 = now_sec();
		times[r] = t1 - t0;
	}

	std::sort(times, times + BENCH_REPS);
	verify_objects(name, buf.data(), (int)buf.size(), key_fn);
	return times[BENCH_REPS / 2];
}

template <typename KeyFn>
static double bench_ptrs(const char *name,
		void (*sort_fn)(HeapObject **, int, KeyFn),
		const std::vector<HeapObject> &storage,
		KeyFn key_fn) {
	std::vector<HeapObject *> ptrs(storage.size());
	double times[BENCH_REPS];

	auto reset_ptrs = [&]() {
		for (size_t i = 0; i < storage.size(); i++) ptrs[i] = const_cast<HeapObject *>(&storage[i]);
	};

	for (int r = 0; r < WARMUP_REPS; r++) {
		reset_ptrs();
		sort_fn(ptrs.data(), (int)ptrs.size(), key_fn);
	}
	for (int r = 0; r < BENCH_REPS; r++) {
		reset_ptrs();
		double t0 = now_sec();
		sort_fn(ptrs.data(), (int)ptrs.size(), key_fn);
		double t1 = now_sec();
		times[r] = t1 - t0;
	}

	std::sort(times, times + BENCH_REPS);
	verify_ptrs(name, ptrs.data(), (int)ptrs.size(), key_fn);
	return times[BENCH_REPS / 2];
}

template <typename KeyFn>
static void run_suite(const char *suite_name, KeyFn key_fn) {
	int sizes[] = { 10000, 100000, 1000000 };
	int range   = 100000;
	const char *dist_names[] = { "Uniform", "Gaussian", "Sorted", "FewVals" };

	std::printf("\n══════════════════════════════════════════════════════════════════════════════════════\n");
	std::printf("  %s\n", suite_name);
	std::printf("══════════════════════════════════════════════════════════════════════════════════════\n");

	for (int si = 0; si < 3; si++) {
		int n = sizes[si];
		std::printf("\n  n = %-7d  |  Results in (ms) - MEDIAN\n", n);
		std::printf("  %-18s %10s %10s %10s %10s\n", "Algorithm", dist_names[0], dist_names[1], dist_names[2], dist_names[3]);
		std::printf("  ");
		for (int x = 0; x < 18 + 4 * 11; x++) std::putchar('-');
		std::printf("\n");

		std::vector<HeapObject> sources[4];
		rng_reset(); sources[0] = make_uniform(n, range);
		rng_reset(); sources[1] = make_gaussian(n, range);
		rng_reset(); sources[2] = make_sorted(n, range);
		rng_reset(); sources[3] = make_few_values(n);

		std::printf("  %-18s", "std::sort (objs)");
		for (int di = 0; di < 4; di++) {
			double t = bench_objects("std::sort (objs)", sources[di], key_fn);
			std::printf(" %10.6f", t * 1000.0);
		}
		std::printf("\n");

		std::printf("  %-18s", "std::sort (ptrs)");
		for (int di = 0; di < 4; di++) {
			double t = bench_ptrs("std::sort (ptrs)", std_sort_ptrs<KeyFn>, sources[di], key_fn);
			std::printf(" %10.6f", t * 1000.0);
		}
		std::printf("\n");

		std::printf("  %-18s", "Canon Sort (ptrs)");
		for (int di = 0; di < 4; di++) {
			double t = bench_ptrs("Canon Sort (ptrs)", canon_sort_ptrs_wrapper<KeyFn>, sources[di], key_fn);
			std::printf(" %10.6f", t * 1000.0);
		}
		std::printf("\n");
	}
}

int main(void) {
	std::printf("Canon Sort Indirect Pointer Benchmark\n");
	std::printf("Warmup: %d  |  Bench reps: %d  |  Metric: MEDIAN run (ms)\n", WARMUP_REPS, BENCH_REPS);
	std::printf("`std::sort (objs)` moves full objects; the pointer variants only reorder the pointer array.\n");
	std::printf("Canon sort precomputes and caches keys once per object before sorting pointers.\n");

	run_suite("Cheap key extractor  (return obj.key)", cheap_key);
	run_suite("Expensive key extractor  (mixed fields + payload)", expensive_key);
	return 0;
}
