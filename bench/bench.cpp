/*
 * bench.cpp — Canon Sort benchmark
 *
 * Build (via just):
 *   just bench
 *
 * Build (manually):
 *   zig c++ -O3 -march=native -std=c++17 -ltbb \
 *       -Iinclude -Ibench/ips4o/include \
 *       -o bench src/canon_sort.cpp bench/bench.cpp
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>

#include <algorithm>

/*
 * TBB must be visible before ips4o.hpp so it can detect parallel support.
 * Previously this was pulled in transitively through canon_sort's inline code;
 * now that canon_sort is a separate translation unit we include it explicitly.
 */
#include <tbb/task_group.h>

#include "../include/canon_sort.hpp"
#include "ips4o/include/ips4o.hpp"

/* ------------------------------------------------------------------ */
/* Timing                                                               */
/* ------------------------------------------------------------------ */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

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

static void insertion_sort(int *arr, int n) {
    if (n > 1) insertion_sort_slice(arr, 0, n - 1);
}

/* ------------------------------------------------------------------ */
/* Quicksort (median-of-3 + insertion fallback at 16)                  */
/* ------------------------------------------------------------------ */

static void qs_rec(int *arr, int low, int high) {
    if (high - low < 16) { insertion_sort_slice(arr, low, high); return; }
    int mid = low + (high - low) / 2;
    if (arr[mid]  < arr[low])  swap_int(&arr[mid],  &arr[low]);
    if (arr[high] < arr[low])  swap_int(&arr[high], &arr[low]);
    if (arr[high] < arr[mid])  swap_int(&arr[high], &arr[mid]);
    int pivot = arr[mid];
    swap_int(&arr[mid], &arr[high - 1]);
    int i = low, j = high - 1;
    for (;;) {
        while (arr[++i] < pivot) {}
        while (arr[--j] > pivot) {}
        if (i >= j) break;
        swap_int(&arr[i], &arr[j]);
    }
    swap_int(&arr[i], &arr[high - 1]);
    qs_rec(arr, low, i - 1);
    qs_rec(arr, i + 1, high);
}

static void quick_sort(int *arr, int n) { if (n > 1) qs_rec(arr, 0, n - 1); }

/* ------------------------------------------------------------------ */
/* Radix sort (LSD, 4 x 8-bit passes)                                  */
/* ------------------------------------------------------------------ */

static void radix_sort(int *arr, int n) {
    if (n < 2) return;
    int *buf = (int*)malloc(n * sizeof(int));
    for (int pass = 0; pass < 4; pass++) {
        int shift = pass * 8, counts[256] = {0};
        for (int i = 0; i < n; i++) counts[(arr[i] >> shift) & 0xFF]++;
        for (int i = 1; i < 256; i++) counts[i] += counts[i-1];
        for (int i = n-1; i >= 0; i--) {
            int b = (arr[i] >> shift) & 0xFF;
            buf[--counts[b]] = arr[i];
        }
        memcpy(arr, buf, n * sizeof(int));
    }
    free(buf);
}

/* ------------------------------------------------------------------ */
/* std::sort wrapper (pdqsort in clang/gcc)                            */
/* ------------------------------------------------------------------ */

static void pdq_sort(int *arr, int n) {
    std::sort(arr, arr + n);
}

/* ------------------------------------------------------------------ */
/* IPS4o wrapper                                                        */
/* ------------------------------------------------------------------ */

static void ips4o_sort(int *arr, int n) {
    ips4o::parallel::sort(arr, arr + n);
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
/* Distributions                                                        */
/* ------------------------------------------------------------------ */

static int *make_uniform(int n, int range) {
    int *a = (int*)malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) a[i] = rng_next() % range;
    return a;
}

static int *make_gaussian(int n, int range) {
    int *a = (int*)malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) {
        double u1 = (rng_next() + 1.0) / 4294967296.0;
        double u2 =  rng_next()        / 4294967296.0;
        double z  = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
        double v  = (z / 6.0 + 0.5) * range;
        if (v < 0) v = 0; else if (v >= range) v = range - 1;
        a[i] = (int)v;
    }
    return a;
}

static int *make_few_values(int n) {
    int *a = (int*)malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) a[i] = rng_next() % 8;
    return a;
}

static int cmp_asc(const void *a, const void *b) {
    return (*(int*)a > *(int*)b) - (*(int*)a < *(int*)b);
}

static int *make_sorted(int n, int range) {
    int *a = make_uniform(n, range);
    qsort(a, n, sizeof(int), cmp_asc);
    return a;
}

static int *make_reverse_sorted(int n, int range) {
    int *a = make_sorted(n, range);
    for (int i = 0, j = n-1; i < j; i++, j--) swap_int(&a[i], &a[j]);
    return a;
}

static int *make_pipe_organ(int n, int range) {
    int *a = make_sorted(n, range);
    int *b = (int*)calloc(n, sizeof(int));
    int  half = n / 2;
    for (int i = 0; i < half; i++) { b[2*i] = a[i]; b[2*i+1] = a[n-1-i]; }
    if (n % 2) b[n-1] = a[half];
    free(a);
    return b;
}

/* ------------------------------------------------------------------ */
/* Correctness                                                          */
/* ------------------------------------------------------------------ */

static void verify(const char *name, int *arr, int n) {
    for (int i = 0; i < n-1; i++) {
        if (arr[i] > arr[i+1]) {
            fprintf(stderr, "CORRECTNESS FAILURE: %s at [%d] (%d > %d)\n",
                name, i, arr[i], arr[i+1]);
            exit(1);
        }
    }
}

static void smoke_test(void) {
    typedef void (*sfn)(int *, int);
    struct { const char *name; sfn fn; } algos[] = {
        {"Insertion Sort", insertion_sort},
        {"Quick Sort",     quick_sort},
        {"Radix Sort",     radix_sort},
        {"pdqsort",        pdq_sort},
        {"IPS4o",          ips4o_sort},
        {"Canon Sort",     canon_sort_int},
    };
    int na = sizeof(algos)/sizeof(algos[0]);

    int t0[] = {5,3,8,1,9,2,7,4};
    int t1[] = {1,1,1,1,1,1,1,1};
    int t2[] = {8,7,6,5,4,3,2,1};
    int t3[] = {1,2,3,4,5,6,7,8};
    int t4[] = {42};
    struct { int *d; int n; } cases[] = {{t0,8},{t1,8},{t2,8},{t3,8},{t4,1}};
    int nc = 5;

    rng_reset();
    int *big = make_uniform(10000, 100000);

    for (int a = 0; a < na; a++) {
        for (int c = 0; c < nc; c++) {
            int tmp[8];
            memcpy(tmp, cases[c].d, cases[c].n * sizeof(int));
            algos[a].fn(tmp, cases[c].n);
            verify(algos[a].name, tmp, cases[c].n);
        }
        int *copy = (int*)malloc(10000 * sizeof(int));
        memcpy(copy, big, 10000 * sizeof(int));
        algos[a].fn(copy, 10000);
        verify(algos[a].name, copy, 10000);
        free(copy);
    }
    free(big);
    printf("✓ All correctness checks passed.\n\n");
}

/* ------------------------------------------------------------------ */
/* Benchmark                                                            */
/* ------------------------------------------------------------------ */

#define WARMUP_REPS  3
#define BENCH_REPS  20

typedef void (*sort_fn_t)(int *, int);

static int cmp_double(const void *a, const void *b) {
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}

static double bench_one(sort_fn_t fn, const char *name, const int *src, int n) {
    int *buf = (int*)malloc(n * sizeof(int));
    double times[BENCH_REPS];

    for (int r = 0; r < WARMUP_REPS; r++) {
        memcpy(buf, src, n * sizeof(int));
        fn(buf, n);
    }
    for (int r = 0; r < BENCH_REPS; r++) {
        memcpy(buf, src, n * sizeof(int));
        double t0 = now_sec();
        fn(buf, n);
        double t1 = now_sec();
        times[r] = t1 - t0;
    }

    qsort(times, BENCH_REPS, sizeof(double), cmp_double);
    double median = (BENCH_REPS % 2 == 0)
        ? (times[BENCH_REPS/2 - 1] + times[BENCH_REPS/2]) / 2.0
        :  times[BENCH_REPS/2];

    verify(name, buf, n);
    free(buf);
    return median;
}

static void run_benchmarks(void) {
    int sizes[] = { 1000, 10000, 100000, 1000000, 5000000 };
    int range   = 100000;

    typedef struct { const char *name; sort_fn_t fn; } Algo;
    Algo algos[] = {
        {"Quick Sort (hand)",   quick_sort},
        {"Radix Sort (LSD)",    radix_sort},
        {"pdqsort (std::sort)", pdq_sort},
        {"IPS4o (parallel)",    ips4o_sort},
        {"Canon Sort",          canon_sort_int},
    };
    int na = sizeof(algos)/sizeof(algos[0]);

    const char *dist_names[] = {
        "Uniform", "Gaussian", "Sorted", "RevSorted", "PipeOrgan", "FewVals"
    };
    int nd = 6;

    for (int si = 0; si < 5; si++) {
        int n = sizes[si];
        printf("\n══════════════════════════════════════════════════════════════════════════════════════\n");
        printf("  n = %-7d  |  Results in (ms) - MEDIAN\n", n);
        printf("══════════════════════════════════════════════════════════════════════════════════════\n");

        printf("  %-22s", "Algorithm");
        for (int di = 0; di < nd; di++) printf(" %10s", dist_names[di]);
        printf("\n  ");
        for (int x = 0; x < 22 + nd*11; x++) putchar('-');
        printf("\n");

        for (int a = 0; a < na; a++) {
            printf("  %-22s", algos[a].name);
            for (int di = 0; di < nd; di++) {
                rng_reset();
                int *src;
                switch (di) {
                    case 0: src = make_uniform(n, range);        break;
                    case 1: src = make_gaussian(n, range);       break;
                    case 2: src = make_sorted(n, range);         break;
                    case 3: src = make_reverse_sorted(n, range); break;
                    case 4: src = make_pipe_organ(n, range);     break;
                    default:src = make_few_values(n);            break;
                }
                double t = bench_one(algos[a].fn, algos[a].name, src, n);
                printf(" %10.6f", t * 1000.0);
                free(src);
            }
            printf("\n");
        }
    }
}

int main(void) {
    printf("Canon Sort Benchmark  [IPS4o parallel + pdqsort via std::sort]\n");
    printf("Warmup: %d  |  Bench reps: %d  |  Metric: MEDIAN run (ms)\n\n",
        WARMUP_REPS, BENCH_REPS);
    smoke_test();
    run_benchmarks();
    return 0;
}
