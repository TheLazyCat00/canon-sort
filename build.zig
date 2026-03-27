const std = @import("std");

pub fn build(b: *std.Build) void {
    const target   = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // ----------------------------------------------------------------
    // Shared library — what FFI consumers link against
    // ----------------------------------------------------------------
    const lib = b.addSharedLibrary(.{
        .name     = "canon",
        .target   = target,
        .optimize = optimize,
    });
    lib.addCSourceFile(.{
        .file  = b.path("src/canon_sort.cpp"),
        .flags = &.{ "-O3", "-march=native", "-std=c++17" },
    });
    lib.addIncludePath(b.path("include"));
    lib.linkSystemLibrary("tbb");
    lib.linkLibCpp();
    b.installArtifact(lib);

    // ----------------------------------------------------------------
    // Static library — alternative for users who prefer static linking
    // ----------------------------------------------------------------
    const lib_static = b.addStaticLibrary(.{
        .name     = "canon",
        .target   = target,
        .optimize = optimize,
    });
    lib_static.addCSourceFile(.{
        .file  = b.path("src/canon_sort.cpp"),
        .flags = &.{ "-O3", "-march=native", "-std=c++17" },
    });
    lib_static.addIncludePath(b.path("include"));
    lib_static.linkSystemLibrary("tbb");
    lib_static.linkLibCpp();
    b.installArtifact(lib_static);

    // ----------------------------------------------------------------
    // Benchmark binary
    // ----------------------------------------------------------------
    const bench = b.addExecutable(.{
        .name     = "bench",
        .target   = target,
        .optimize = optimize,
    });
    bench.addCSourceFiles(.{
        .files = &.{
            "src/canon_sort.cpp",
            "bench/bench.cpp",
        },
        .flags = &.{ "-O3", "-march=native", "-std=c++17" },
    });
    bench.addIncludePath(b.path("include"));
    bench.addIncludePath(b.path("bench/ips4o/include"));
    bench.linkSystemLibrary("tbb");
    bench.linkLibCpp();

    const run_bench = b.addRunArtifact(bench);
    const bench_step = b.step("bench", "Build and run the benchmark");
    bench_step.dependOn(&run_bench.step);
    b.installArtifact(bench);
}
