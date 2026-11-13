"""Host tests: GoogleTest cc_tests of ios-on-switch-lmao's own C and C++, built for this Mac by the host C/C++ toolchain.

Each test lives in a `test/` package beside the code it tests (drivers/SwitchTouch/test, boot/loader/test, ...) and
lists only what it compiles or reads, so an edit reruns only the tests that depend on it. Nothing compiles at test time.

    load("//bazel/testing:defs.bzl", "host_library", "host_shims", "host_test")

    # The kernel headers the driver includes, each a one-line header that includes the test's stand-ins.
    host_shims(
        name = "kernel",
        headers = ["IOKit/IOService.h", "IOKit/IOLib.h", "os/log.h"],
        stubs = "switch_touch_stubs.h",
    )

    # One translation unit of real driver code (its own flags), built against those stand-ins.
    host_library(
        name = "i2c",
        srcs = ["//drivers/TegraI2C:TegraI2C.cpp"],
        hdrs = ["//drivers/TegraI2C:TegraI2C.h"],
        defines = ["TEGRA_I2C_HOST_MODEL"],
        force_includes = ["tegra_i2c_host_model.h"],
        deps = [":kernel"],
    )

    host_test(
        name = "switch_touch_test",
        srcs = ["switch_touch_test.cpp"],
        deps = [":i2c", ":kernel"],
    )

Everything is compiled with -Wall -Wextra -Werror and, unless `sanitize = False`, AddressSanitizer and
UndefinedBehaviorSanitizer (no recovery: the first report fails the test). Tests link GoogleTest's main; scenarios that
need a process of their own (drivers keep boot-wide state) use //bazel/testing:fresh_process.h, and checks of source
text use //bazel/testing:source_text.h. Every flag goes through copts/linkopts, so the targets stay toolchain-neutral.
A test that #includes a driver's .cpp itself (one translation unit with its model) lists it in
`additional_compiler_inputs` (passed through to cc_test).
"""

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

WARNINGS = [
    "-Wall",
    "-Wextra",
    "-Werror",
]

SANITIZERS = [
    "-fsanitize=address,undefined",
    "-fno-sanitize-recover=all",
]

# GoogleTest's headers as system headers, so their warnings (1.17's gtest-printers.h trips -Wcharacter-conversion under
# C++20) never fail our -Werror. Its own `includes` reach us as -I; clang drops an -I directory that is also -isystem.
# (The external_include_paths feature below does the same where every target has it on.)
_GTEST_ROOT = Label("@googletest//:gtest").workspace_root
GTEST_SYSTEM_INCLUDES = [
    "-isystem",
    _GTEST_ROOT + "/googletest/include",
    "-isystem",
    _GTEST_ROOT + "/googlemock/include",
]

def _flags(c_std, cxx_std, sanitize, copts, features):
    return struct(
        conlyopts = ["-std=" + c_std],
        cxxopts = ["-std=" + cxx_std] + GTEST_SYSTEM_INCLUDES,
        copts = WARNINGS + (SANITIZERS if sanitize else []) + ["-g"] + copts,
        linkopts = SANITIZERS if sanitize else [],
        # Toolchain features as well as the flags: a toolchain that knows "asan"/"ubsan" then leaves out what conflicts
        # with them (apple_support's -D_FORTIFY_SOURCE=1); one that does not ignores the names. external_include_paths
        # puts other repositories' headers (GoogleTest's) on -isystem, so their warnings never fail our -Werror.
        features = (["asan", "ubsan"] if sanitize else []) + ["external_include_paths"] + features,
    )

def _force_includes(force_includes):
    # Package-relative headers, forced into every unit of the target (-include takes an execroot-relative path).
    package = native.package_name()
    flags = []
    for header in force_includes:
        flags += ["-include", package + "/" + header if package else header]
    return flags

def host_library(
        name,
        srcs = [],
        hdrs = [],
        deps = [],
        copts = [],
        defines = [],
        force_includes = [],
        c_std = "c11",
        cxx_std = "c++20",
        sanitize = True,
        features = [],
        **kwargs):
    """Code under test (a driver's own .cpp/.c, a model), built for the host with the tests' flags.

    Args:
        name: the cc_library's name.
        srcs: the translation units (labels of the real sources) and any private headers.
        hdrs: headers the unit's users include.
        deps: other host_library/host_shims targets.
        copts: extra compiler flags for these units only.
        defines: preprocessor definitions for these units only (local_defines: they do not reach dependants).
        force_includes: headers of this package forced into each unit (-include), e.g. a register model's hooks.
        c_std: the C standard for .c units.
        cxx_std: the C++ standard for .cpp units.
        sanitize: build with ASan + UBSan (the test must link them too).
        features: extra toolchain features.
        **kwargs: passed to cc_library.
    """
    flags = _flags(c_std, cxx_std, sanitize, copts + _force_includes(force_includes), features)
    cc_library(
        name = name,
        srcs = srcs + force_includes,
        hdrs = hdrs,
        deps = deps,
        conlyopts = flags.conlyopts,
        cxxopts = flags.cxxopts,
        copts = flags.copts,
        local_defines = defines,
        features = flags.features,
        testonly = True,
        # Units of driver code are linked whole: a model's or a driver's static constructors must not be dropped.
        alwayslink = True,
        **kwargs
    )

def host_test(
        name,
        srcs,
        deps = [],
        data = [],
        copts = [],
        defines = [],
        linkopts = [],
        c_std = "c11",
        cxx_std = "c++20",
        sanitize = True,
        size = "small",
        gtest_main = True,
        debug_only = False,
        features = [],
        **kwargs):
    """A GoogleTest cc_test for the host.

    Args:
        name: the test's name (ends in _test).
        srcs: the test's own sources: its TESTs, models and stand-ins.
        deps: host_library/host_shims targets with the code under test.
        data: files the test reads at run time (source_text.h's SourceText reads them by package-relative path).
        copts: extra compiler flags for the test's own sources.
        defines: preprocessor definitions for the test's own sources.
        linkopts: extra link flags (an SDK library, say: -lz).
        c_std: the C standard for .c sources.
        cxx_std: the C++ standard for .cpp sources.
        sanitize: build and link with ASan + UBSan.
        size: the test size (small: under a minute).
        gtest_main: link GoogleTest's main (False: the test defines main itself).
        debug_only: the test needs assertions of the code under test to fire (assert(), compiled out by NDEBUG under
            --config=release): skipped under //bazel:release. Checks of the test itself never need it (REQUIRE).
        features: extra toolchain features.
        **kwargs: passed to cc_test.
    """
    flags = _flags(c_std, cxx_std, sanitize, copts, features)
    if sanitize:
        # GoogleTest (and libc++'s inline code it instantiates) is not built with ASan, while the test's own units are:
        # libc++'s container annotations then disagree between the two copies of a std::vector function the linker
        # keeps, and ASan reports an overflow in GoogleTest's own test registration (seen under --config=release).
        # Every other ASan check stays on; the code under test is kernel C/C++ without std containers.
        # Driver and kit code also gets ASan's stack-use-after-return check: a pointer kept into a frame that returned
        # (a use array an ownership transaction keeps past its caller's return) faults in the host test instead of
        # reading whatever the stack holds by then.
        options = "detect_container_overflow=0"
        if native.package_name().startswith(("drivers/", "kits/")):
            options += ":detect_stack_use_after_return=1"
        kwargs["env"] = dict({"ASAN_OPTIONS": options}, **kwargs.get("env", {}))
    if debug_only:
        kwargs["target_compatible_with"] = kwargs.get("target_compatible_with", []) + select({
            Label("//bazel:release"): ["@platforms//:incompatible"],
            "//conditions:default": [],
        })
    cc_test(
        name = name,
        srcs = srcs,
        deps = deps + [
            Label("//bazel/testing"),
            Label("@googletest//:gtest_main" if gtest_main else "@googletest//:gtest"),
        ],
        data = data,
        conlyopts = flags.conlyopts,
        cxxopts = flags.cxxopts,
        copts = flags.copts,
        linkopts = flags.linkopts + linkopts,
        # source_text.h resolves package-relative paths through this (the test's package, so a directory move that
        # carries the test along keeps working).
        local_defines = defines + ['SWITCH_TEST_PACKAGE=\\"%s\\"' % native.package_name()],
        features = flags.features,
        size = size,
        **kwargs
    )

def _shims_impl(ctx):
    stubs = ctx.file.stubs.short_path
    outputs = []
    for header in ctx.attr.headers:
        output = ctx.actions.declare_file(ctx.label.name + "_include/" + header)
        ctx.actions.write(output, "// Generated by //bazel/testing:defs.bzl host_shims (%s).\n#include \"%s\"\n" % (
            ctx.label,
            stubs,
        ))
        outputs.append(output)
    return [DefaultInfo(files = depset(outputs))]

_shims = rule(
    implementation = _shims_impl,
    doc = "One-line headers at the kernel's include paths, each including the test's stand-ins.",
    attrs = {
        "headers": attr.string_list(mandatory = True, doc = "Include paths, e.g. IOKit/IOService.h."),
        "stubs": attr.label(allow_single_file = [".h"], mandatory = True, doc = "The stand-ins they include."),
    },
)

def host_shims(name, headers, stubs, deps = [], **kwargs):
    """The kernel's headers (<IOKit/...>, <os/log.h>, ...) for host builds of driver code, all one stand-in header.

    `<name>` is a cc_library whose include path holds each of `headers` as a one-line header including `stubs`
    (by its workspace path); depend on it from the host_library of the driver and from the test.

    Args:
        name: the cc_library's name.
        headers: the include paths the driver uses (<IOKit/IOService.h> is "IOKit/IOService.h").
        stubs: the test's stand-in header (a file of this package).
        deps: headers the stand-ins include.
        **kwargs: passed to cc_library.
    """
    _shims(
        name = name + "_headers",
        headers = headers,
        stubs = stubs,
        testonly = True,
    )
    cc_library(
        name = name,
        hdrs = [":" + name + "_headers", stubs],
        includes = [name + "_headers_include"],
        deps = deps,
        testonly = True,
        **kwargs
    )
