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

targets := "x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl"

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

# Compile TBB sources directly with zig — no CMake needed for cross targets
tbb-cross target:
	#!/usr/bin/env sh
	set -e
	OBJ_DIR="$(pwd)/vendor/tbb/build/{{target}}/tbb_objects"
	rm -rf "$OBJ_DIR"
	mkdir -p "$OBJ_DIR"

	TBB_SRC="$(pwd)/vendor/tbb/src/tbb"
	TBB_INC="$(pwd)/vendor/tbb/include"
	TBB_SRC_INC="$(pwd)/vendor/tbb/src"

	DEFINES="-D__TBB_BUILD \
		-D__TBB_DYNAMIC_LOAD_ENABLED=0 \
		-D__TBB_SOURCE_DIRECTLY_INCLUDED=1 \
		-DTBB_USE_THREADING_TOOLS \
		-D_FORTIFY_SOURCE=2"

	case "{{target}}" in
	x86_64*) ARCH_FLAGS="-mwaitpkg" ;;
	*)       ARCH_FLAGS="" ;;
	esac

	CXX_FLAGS="-O2 -std=c++17 -fPIC -target {{target}} $ARCH_FLAGS"

	SOURCES="
	$TBB_SRC/address_waiter.cpp
	$TBB_SRC/allocator.cpp
	$TBB_SRC/arena.cpp
	$TBB_SRC/arena_slot.cpp
	$TBB_SRC/concurrent_bounded_queue.cpp
	$TBB_SRC/dynamic_link.cpp
	$TBB_SRC/exception.cpp
	$TBB_SRC/global_control.cpp
	$TBB_SRC/governor.cpp
	$TBB_SRC/itt_notify.cpp
	$TBB_SRC/main.cpp
	$TBB_SRC/market.cpp
	$TBB_SRC/misc.cpp
	$TBB_SRC/misc_ex.cpp
	$TBB_SRC/observer_proxy.cpp
	$TBB_SRC/parallel_pipeline.cpp
	$TBB_SRC/private_server.cpp
	$TBB_SRC/profiling.cpp
	$TBB_SRC/queuing_rw_mutex.cpp
	$TBB_SRC/rml_tbb.cpp
	$TBB_SRC/rtm_mutex.cpp
	$TBB_SRC/rtm_rw_mutex.cpp
	$TBB_SRC/semaphore.cpp
	$TBB_SRC/small_object_pool.cpp
	$TBB_SRC/task.cpp
	$TBB_SRC/task_dispatcher.cpp
	$TBB_SRC/task_group_context.cpp
	$TBB_SRC/tcm_adaptor.cpp
	$TBB_SRC/thread_dispatcher.cpp
	$TBB_SRC/thread_request_serializer.cpp
	$TBB_SRC/threading_control.cpp
	$TBB_SRC/version.cpp
	"

	echo "Compiling TBB for {{target}}..."
	for src in $SOURCES; do
	obj="$OBJ_DIR/$(basename "$src" .cpp).o"
	{{zig_cxx}} $CXX_FLAGS $DEFINES \
		-I"$TBB_INC" -I"$TBB_SRC_INC" \
		-c "$src" -o "$obj"
	done
	echo "→ TBB objects in $OBJ_DIR"

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
