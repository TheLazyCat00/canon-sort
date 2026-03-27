#pragma once

/*
 * canon_sort_int — sort an array of int in ascending order.
 *
 * The __asm__ attribute pins the exported symbol name so C++ name mangling
 * never affects FFI callers (OCaml, Python, Rust, …).
 *
 * Compile with zig cc / clang++ / g++:
 *   zig c++ -O3 -march=native -ltbb -o canon_sort.o -c canon_sort.cpp
 */
void canon_sort_int(int *arr, int n) __asm__("canon_sort_int");
