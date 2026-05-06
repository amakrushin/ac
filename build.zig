const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const c_flags = &[_][]const u8{
        "-std=c2x",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Wno-unused-parameter",
        "-D_GNU_SOURCE",
    };

    // ac executable
    const ac = b.addExecutable(.{
        .name = "ac",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    ac.root_module.addCSourceFiles(.{
        .files = &.{
            "src/main.c",
            "src/termbox2_impl.c",
            "src/ui_dashboard.c",
            "src/ring.c",
            "src/bus.c",
            "src/log.c",
            "src/proc_posix.c",
            "src/events_cc.c",
            "src/agent.c",
            "src/agent_session.c",
            "third_party/cjson/cJSON.c",
        },
        .flags = c_flags,
    });
    ac.root_module.addIncludePath(b.path("third_party/termbox2"));
    ac.root_module.addIncludePath(b.path("third_party/cjson"));
    b.installArtifact(ac);

    const run_cmd = b.addRunArtifact(ac);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| run_cmd.addArgs(args);
    const run_step = b.step("run", "Run ac");
    run_step.dependOn(&run_cmd.step);

    // ac_test executable (Catch-style C tests via greatest)
    const ac_test = b.addExecutable(.{
        .name = "ac_test",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    ac_test.root_module.addCSourceFiles(.{
        .files = &.{
            "tests/test_main.c",
            "tests/test_ring.c",
            "tests/test_proc.c",
            "tests/test_events_cc.c",
            "tests/test_agent.c",
            "tests/test_bus.c",
            "tests/test_agent_session.c",
            "src/ring.c",
            "src/proc_posix.c",
            "src/events_cc.c",
            "src/agent.c",
            "src/bus.c",
            "src/agent_session.c",
            "third_party/cjson/cJSON.c",
        },
        .flags = c_flags,
    });
    ac_test.root_module.addIncludePath(b.path("third_party/greatest"));
    ac_test.root_module.addIncludePath(b.path("third_party/cjson"));
    ac_test.root_module.addIncludePath(b.path("src"));

    const run_test = b.addRunArtifact(ac_test);
    run_test.step.dependOn(b.getInstallStep());
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_test.step);
}
