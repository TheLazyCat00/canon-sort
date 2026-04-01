// SPDX-License-Identifier: MIT
#pragma once

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
}
