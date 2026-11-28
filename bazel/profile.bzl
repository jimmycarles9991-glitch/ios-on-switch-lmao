"""//bazel:profile as user-space C, C++ and Objective-C++ see it: SWITCH_DEBUG is 1 in debug and 0 in release, and 0 in
either profile under --//bazel:diagnostics=false.

Kernel code needs no define of its own: XNU's DEVELOPMENT is defined exactly in the debug profile (kernel/rules/defs.bzl
maps debug to XNU DEVELOPMENT and release to RELEASE), and kernel_driver undefines it under --//bazel:diagnostics=false."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

SWITCH_DEBUG_DEFINES = {
    "debug": "-DSWITCH_DEBUG=1",
    "release": "-DSWITCH_DEBUG=0",
}

PROFILE_ATTR = {
    "_profile": attr.label(default = "//bazel:profile"),
    "_diagnostics": attr.label(default = "//bazel:diagnostics"),
}

METAL_STATS_ATTR = {
    "_stats_gpu": attr.label(default = "//bazel:stats_gpu"),
    "_prof_gpu": attr.label(default = "//bazel:prof_gpu"),
    "_stats_frame": attr.label(default = "//bazel:stats_frame"),
}

def switch_debug_define(ctx):
    """The -D flag for the profile and diagnostics settings ctx.attr._profile and ._diagnostics (PROFILE_ATTR) name."""
    if not ctx.attr._diagnostics[BuildSettingInfo].value:
        return SWITCH_DEBUG_DEFINES["release"]
    return SWITCH_DEBUG_DEFINES[ctx.attr._profile[BuildSettingInfo].value]

def metal_stats_defines(ctx):
    """Per-category opt-in -D flags for the Metal plugin (0 unless debug + diagnostics + category flag)."""
    debug_on = (
        ctx.attr._profile[BuildSettingInfo].value == "debug" and
        ctx.attr._diagnostics[BuildSettingInfo].value
    )
    flags = [
        ("_stats_gpu", "SWITCH_STATS_GPU"),
        ("_prof_gpu", "SWITCH_PROF_GPU"),
        ("_stats_frame", "SWITCH_STATS_FRAME"),
    ]
    return [
        "-D{}={}".format(macro, 1 if (debug_on and getattr(ctx.attr, attr_name)[BuildSettingInfo].value) else 0)
        for attr_name, macro in flags
    ]
