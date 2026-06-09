// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

/*
 * canon_sort — sort arrays of any fixed-width numeric type.
 *
 * Each function takes a raw void* and element count.
 * Caller is responsible for passing the correct function for their type.
 *
 * NaN behaviour is undefined for f32/f64.
 */

/* type tags for canon_sort dispatcher */
#define CANON_U8   1
#define CANON_U16  2
#define CANON_U32  4
#define CANON_U64  8

extern "C" {
	/* typed entry points — zero dispatch overhead */
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

	/* convenience dispatcher */
	void canon_sort(void *ptr, int n, int type_size);

	/*
	 * indirect pointer sorting using precomputed numeric keys.
	 *
	 * `ptrs` points to an array of object pointers (e.g. T**).
	 * `keys` must contain one numeric key per pointer.
	 * the pointer array is reordered by key; equal-key order is unspecified.
	 */
	void canon_sort_ptrs_i8 (void *ptrs, const int8_t   *keys, int n);
	void canon_sort_ptrs_i16(void *ptrs, const int16_t  *keys, int n);
	void canon_sort_ptrs_i32(void *ptrs, const int32_t  *keys, int n);
	void canon_sort_ptrs_i64(void *ptrs, const int64_t  *keys, int n);
	void canon_sort_ptrs_u8 (void *ptrs, const uint8_t  *keys, int n);
	void canon_sort_ptrs_u16(void *ptrs, const uint16_t *keys, int n);
	void canon_sort_ptrs_u32(void *ptrs, const uint32_t *keys, int n);
	void canon_sort_ptrs_u64(void *ptrs, const uint64_t *keys, int n);
	void canon_sort_ptrs_f32(void *ptrs, const float    *keys, int n);
	void canon_sort_ptrs_f64(void *ptrs, const double   *keys, int n);
}

#ifdef __cplusplus
#include <type_traits>
#include <utility>
#include <vector>

namespace canon_sort_detail {
	template <typename Key> struct supported_key : std::false_type {};
	template <> struct supported_key<int8_t>   : std::true_type {};
	template <> struct supported_key<int16_t>  : std::true_type {};
	template <> struct supported_key<int32_t>  : std::true_type {};
	template <> struct supported_key<int64_t>  : std::true_type {};
	template <> struct supported_key<uint8_t>  : std::true_type {};
	template <> struct supported_key<uint16_t> : std::true_type {};
	template <> struct supported_key<uint32_t> : std::true_type {};
	template <> struct supported_key<uint64_t> : std::true_type {};
	template <> struct supported_key<float>    : std::true_type {};
	template <> struct supported_key<double>   : std::true_type {};

	inline void sort_ptrs_cached(void *ptrs, const int8_t *keys, int n)   { canon_sort_ptrs_i8 (ptrs, keys, n); }
	inline void sort_ptrs_cached(void *ptrs, const int16_t *keys, int n)  { canon_sort_ptrs_i16(ptrs, keys, n); }
	inline void sort_ptrs_cached(void *ptrs, const int32_t *keys, int n)  { canon_sort_ptrs_i32(ptrs, keys, n); }
	inline void sort_ptrs_cached(void *ptrs, const int64_t *keys, int n)  { canon_sort_ptrs_i64(ptrs, keys, n); }
	inline void sort_ptrs_cached(void *ptrs, const uint8_t *keys, int n)  { canon_sort_ptrs_u8 (ptrs, keys, n); }
	inline void sort_ptrs_cached(void *ptrs, const uint16_t *keys, int n) { canon_sort_ptrs_u16(ptrs, keys, n); }
	inline void sort_ptrs_cached(void *ptrs, const uint32_t *keys, int n) { canon_sort_ptrs_u32(ptrs, keys, n); }
	inline void sort_ptrs_cached(void *ptrs, const uint64_t *keys, int n) { canon_sort_ptrs_u64(ptrs, keys, n); }
	inline void sort_ptrs_cached(void *ptrs, const float *keys, int n)    { canon_sort_ptrs_f32(ptrs, keys, n); }
	inline void sort_ptrs_cached(void *ptrs, const double *keys, int n)   { canon_sort_ptrs_f64(ptrs, keys, n); }
} // namespace canon_sort_detail

/* sort an array of object pointers using precomputed numeric keys */
template <typename Obj, typename Key>
inline void canon_sort_ptrs_by_cached_keys(Obj **ptrs, const Key *keys, int n) {
	static_assert(canon_sort_detail::supported_key<std::decay_t<Key>>::value,
		"canon_sort_ptrs_by_cached_keys: key type must be one of i8/i16/i32/i64/u8/u16/u32/u64/f32/f64");
	canon_sort_detail::sort_ptrs_cached(static_cast<void *>(ptrs), keys, n);
}

/*
 * sort an array of object pointers by extracting and caching a numeric key.
 *
 * example:
 *   canon_sort_ptrs_by(objs.data(), n, [](const Obj &o) { return o.score; });
 */
template <typename Obj, typename KeyFn>
inline void canon_sort_ptrs_by(Obj **ptrs, int n, KeyFn &&key_fn) {
	if (n < 2) return;
	using Key = std::decay_t<decltype(std::declval<KeyFn &>()(*ptrs[0]))>;
	static_assert(canon_sort_detail::supported_key<Key>::value,
		"canon_sort_ptrs_by: key function must return one of i8/i16/i32/i64/u8/u16/u32/u64/f32/f64");

	std::vector<Key> keys;
	keys.reserve((size_t)n);
	for (int i = 0; i < n; i++) keys.push_back(key_fn(*ptrs[i]));
	canon_sort_detail::sort_ptrs_cached(static_cast<void *>(ptrs), keys.data(), n);
}
#endif
