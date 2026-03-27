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
#define CANON_I8   1
#define CANON_I16  2
#define CANON_I32  4
#define CANON_I64  8

/* typed entry points — zero dispatch overhead */
void canon_sort_i8 (void *ptr, int n) __asm__("canon_sort_i8");
void canon_sort_i16(void *ptr, int n) __asm__("canon_sort_i16");
void canon_sort_i32(void *ptr, int n) __asm__("canon_sort_i32");
void canon_sort_i64(void *ptr, int n) __asm__("canon_sort_i64");
void canon_sort_u8 (void *ptr, int n) __asm__("canon_sort_u8");
void canon_sort_u16(void *ptr, int n) __asm__("canon_sort_u16");
void canon_sort_u32(void *ptr, int n) __asm__("canon_sort_u32");
void canon_sort_u64(void *ptr, int n) __asm__("canon_sort_u64");
void canon_sort_f32(void *ptr, int n) __asm__("canon_sort_f32");
void canon_sort_f64(void *ptr, int n) __asm__("canon_sort_f64");

/* convenience dispatcher */
void canon_sort(void *ptr, int n, int type_size) __asm__("canon_sort");
