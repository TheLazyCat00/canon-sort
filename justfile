# ----------------------------------------------------------------
# Variables
# ----------------------------------------------------------------

cxx       := "zig c++"
flags     := "-O3 -march=native -std=c++17 -pthread"
inc       := "-Iinclude"
inc_bench := "-Iinclude -I./bench/ips4o/include"
libs      := "-ltbb"

# ----------------------------------------------------------------
# Default: list recipes
# ----------------------------------------------------------------

default:
	@just --list

# ----------------------------------------------------------------
# Build shared library  →  libcanon.so
# ----------------------------------------------------------------

shared:
	{{cxx}} {{flags}} {{inc}} {{libs}} \
		-shared -fPIC \
		-o libcanon.so \
		src/canon_sort.cpp

# ----------------------------------------------------------------
# Build static library  →  libcanon.a
# ----------------------------------------------------------------

static:
	{{cxx}} {{flags}} {{inc}} {{libs}} \
		-c -o canon_sort.o src/canon_sort.cpp
	zig ar rcs libcanon.a canon_sort.o
	rm canon_sort.o

# ----------------------------------------------------------------
# Build benchmark binary  →  build/bench
# ----------------------------------------------------------------

bench:
	mkdir -p build
	{{cxx}} {{flags}} {{inc_bench}} {{libs}} \
		-o build/bench \
		src/canon_sort.cpp bench/bench.cpp

# ----------------------------------------------------------------
# Build + run benchmark
# ----------------------------------------------------------------

run: bench
	./build/bench

# ----------------------------------------------------------------
# Cross-compile shared library (example: aarch64 linux musl)
# ----------------------------------------------------------------

cross target="aarch64-linux-musl":
	{{cxx}} {{flags}} {{inc}} {{libs}} \
		-shared -fPIC \
		-target {{target}} \
		-o libcanon-{{target}}.so \
		src/canon_sort.cpp

# ----------------------------------------------------------------
# Clean
# ----------------------------------------------------------------

clean:
	rm -rf build libcanon.so libcanon.a libcanon-*.so
