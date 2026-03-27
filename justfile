# canon_sort — just recipes
# https://github.com/casey/just

# ----------------------------------------------------------------
# Variables
# ----------------------------------------------------------------

zig_cc  := `pwd` + "/scripts/zig-cc"
zig_cxx := `pwd` + "/scripts/zig-cxx"
zig_ar  := `pwd` + "/scripts/zig-ar"

flags       := "-O3 -march=native -std=c++17 -pthread"
flags_cross := "-O3 -std=c++17 -pthread"
inc         := "-Iinclude"
inc_bench   := "-Iinclude -I./bench/ips4o/include"

targets := "x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl x86_64-windows-gnu aarch64-windows-gnu"

# ----------------------------------------------------------------
# Default: list recipes
# ----------------------------------------------------------------

default:
	@just --list

# ----------------------------------------------------------------
# Build TBB for host (shared — static TBB is officially unsupported)
# →  vendor/tbb/build/host/libtbb.so
# ----------------------------------------------------------------

tbb-host:
	cmake -S vendor/tbb -B vendor/tbb/build/host \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_CXX_COMPILER="{{zig_cxx}}" \
		-DCMAKE_C_COMPILER="{{zig_cc}}" \
		-DCMAKE_AR="{{zig_ar}}" \
		-DBUILD_SHARED_LIBS=ON \
		-DTBB_TEST=OFF \
		-DTBB_STRICT=OFF \
		-DTBBMALLOC_BUILD=OFF \
		-DTBBMALLOC_PROXY_BUILD=OFF
	cmake --build vendor/tbb/build/host --parallel

# ----------------------------------------------------------------
# Build TBB for a cross target (static objects, merged into libcanon.a)
# →  vendor/tbb/build/<target>/tbb_objects/
# ----------------------------------------------------------------

tbb-cross target:
	#!/usr/bin/env sh
	set -e
	BUILD_DIR="$(pwd)/vendor/tbb/build/{{target}}"
	rm -rf "$BUILD_DIR"
	mkdir -p "$BUILD_DIR"
	TOOLCHAIN="$BUILD_DIR/toolchain.cmake"

	case "{{target}}" in
	*windows*) SYSTEM_NAME=Windows ;;
	*linux*)   SYSTEM_NAME=Linux   ;;
	*macos*)   SYSTEM_NAME=Darwin  ;;
	*)         SYSTEM_NAME=Linux   ;;
	esac

	case "{{target}}" in
	x86_64*) TBB_ARCH_FLAGS="-mwaitpkg" ;;
	*)       TBB_ARCH_FLAGS="" ;;
	esac

	cat > "$TOOLCHAIN" << TOOLCHAIN_EOF
	set(CMAKE_SYSTEM_NAME ${SYSTEM_NAME})
	set(CMAKE_C_COMPILER "{{zig_cc}}")
	set(CMAKE_CXX_COMPILER "{{zig_cxx}}")
	set(CMAKE_AR "{{zig_ar}}")
	set(CMAKE_C_COMPILER_TARGET "{{target}}")
	set(CMAKE_CXX_COMPILER_TARGET "{{target}}")
	set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
	set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
	set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
	TOOLCHAIN_EOF

	cmake -S "$(pwd)/vendor/tbb" -B "$BUILD_DIR" \
	-G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
	-DCMAKE_CXX_FLAGS="$TBB_ARCH_FLAGS" \
	-DBUILD_SHARED_LIBS=OFF \
	-DTBB_TEST=OFF \
	-DTBB_STRICT=OFF \
	-DTBBMALLOC_BUILD=OFF \
	-DTBBMALLOC_PROXY_BUILD=OFF \
	-DTBB_DISABLE_HWLOC_AUTOMATIC_SEARCH=ON
	cmake --build "$BUILD_DIR" --parallel

	TBB_A="$(find "$BUILD_DIR" -name 'libtbb.a' | head -1)"
	if [ -z "$TBB_A" ]; then
	echo "ERROR: could not find libtbb.a under $BUILD_DIR" >&2
		exit 1
		fi
		echo "Found: $TBB_A"

	mkdir -p "$BUILD_DIR/tbb_objects"
	cd "$BUILD_DIR/tbb_objects"
	{{zig_ar}} x "$TBB_A"

# ----------------------------------------------------------------
# Build static library for host  →  libcanon.a
# (links against shared libtbb at runtime — set LD_LIBRARY_PATH
#  or install libtbb.so to use)
# ----------------------------------------------------------------

build: tbb-host
	zig c++ {{flags}} {{inc}} \
		-Ivendor/tbb/include \
		-c -o canon_sort.o src/canon_sort.cpp
	zig ar rcs libcanon.a canon_sort.o
	rm canon_sort.o

# ----------------------------------------------------------------
# Build benchmark binary and run it
# ----------------------------------------------------------------

bench: tbb-host
	mkdir -p build
	zig c++ {{flags}} {{inc_bench}} \
		-Ivendor/tbb/include \
		-L vendor/tbb/build/host \
		-Wl,-rpath,vendor/tbb/build/host \
		-o build/bench \
		src/canon_sort.cpp bench/bench.cpp \
		-ltbb
	./build/bench

# ----------------------------------------------------------------
# Cross-compile canon_sort for a single target
# Produces a fat libcanon.a with TBB objects merged in
# →  dist/<target>/libcanon.a
# ----------------------------------------------------------------

cross target: (tbb-cross target)
	mkdir -p dist/{{target}}
	zig c++ {{flags_cross}} {{inc}} \
		-Ivendor/tbb/include \
		-target {{target}} \
		-c -o dist/{{target}}/canon_sort.o \
		src/canon_sort.cpp
	# Merge canon_sort.o + all TBB objects into one fat archive
	zig ar rcs dist/{{target}}/libcanon.a \
		dist/{{target}}/canon_sort.o \
		vendor/tbb/build/{{target}}/tbb_objects/*.o
	rm dist/{{target}}/canon_sort.o
	@echo "→ dist/{{target}}/libcanon.a"

# ----------------------------------------------------------------
# Cross-compile for all targets
# ----------------------------------------------------------------

cross-all:
	@for t in {{targets}}; do just cross $t; done

# ----------------------------------------------------------------
# Clean build artifacts (keeps vendored TBB builds)
# ----------------------------------------------------------------

clean:
	rm -rf build dist libcanon.a

# ----------------------------------------------------------------
# Full clean including TBB builds
# ----------------------------------------------------------------

clean-all:
	rm -rf build dist libcanon.a vendor/tbb/build
