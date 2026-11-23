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

def switch_debug_define(ctx):
    """The -D flag for the profile and diagnostics settings ctx.attr._profile and ._diagnostics (PROFILE_ATTR) name."""
    if not ctx.attr._diagnostics[BuildSettingInfo].value:
        return SWITCH_DEBUG_DEFINES["release"]
    return SWITCH_DEBUG_DEFINES[ctx.attr._profile[BuildSettingInfo].value]
