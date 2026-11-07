"""Analysis tests of ipsw_free: a copy of an IPSW member is refused, packaged or not; a check that only reads one passes;
of the files made from the IPSW, only the declassified ones pass."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("@rules_pkg//pkg:mappings.bzl", "pkg_files")
load("@rules_pkg//pkg:zip.bzl", "pkg_zip")
load(":ipsw_free.bzl", "ipsw_free")

def _checked_copy_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".txt")
    ctx.actions.write(out, "made without the checked file\n")
    stamp = ctx.actions.declare_file(ctx.label.name + ".ok")
    ctx.actions.run_shell(
        command = '/bin/test -s "$1" && echo ok > "$2"',
        arguments = [ctx.file.check.path, stamp.path],
        inputs = [ctx.file.check],
        outputs = [stamp],
    )
    return [DefaultInfo(files = depset([out])), OutputGroupInfo(_validation = depset([stamp]))]

_checked_copy = rule(
    implementation = _checked_copy_impl,
    attrs = {"check": attr.label(allow_single_file = True, mandatory = True)},
)

def _refused_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, "would ship Apple's bytes")
    asserts.expect_failure(env, "a file of @@+ipsw+ipsw_")
    return analysistest.end(env)

_refused_test = analysistest.make(_refused_impl, expect_failure = True)

def _passed_impl(ctx):
    env = analysistest.begin(ctx)
    files = analysistest.target_under_test(env)[DefaultInfo].files.to_list()
    asserts.equals(env, ["validation_only.txt"], [f.basename for f in files])
    return analysistest.end(env)

_passed_test = analysistest.make(_passed_impl)

def _window_refused_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, "would ship Apple's bytes")
    asserts.expect_failure(env, "kernel-window.bin")
    return analysistest.end(env)

_window_refused_test = analysistest.make(_window_refused_impl, expect_failure = True)

def _declassified_impl(ctx):
    env = analysistest.begin(ctx)
    files = analysistest.target_under_test(env)[DefaultInfo].files.to_list()
    asserts.equals(env, ["constants.json", "loader.uboot.bin"], sorted([f.basename for f in files]))
    return analysistest.end(env)

_declassified_test = analysistest.make(_declassified_impl)

def ipsw_free_tests(name, member, declassified, window):
    """The tests, as one test_suite.

    Args:
        name: the test_suite's name.
        member: the label of a small IPSW member's file.
        declassified: the targets of ipsw_free.bzl's DECLASSIFIED files (the loader and its constants).
        window: a target whose files were made beside them from the IPSW (the kernel window), which stay refused.
    """
    native.genrule(
        name = "member_copy",
        srcs = [member],
        outs = ["member_copy.bin"],
        cmd = "cp $< $@",
        tags = ["manual"],
    )
    ipsw_free(
        name = "member_copy_checked",
        src = ":member_copy",
        tags = ["manual"],
    )
    _refused_test(
        name = name + "_refuses_a_copy",
        target_under_test = ":member_copy_checked",
    )
    ipsw_free(
        name = "member_copy_file_checked",
        src = ":member_copy.bin",
        tags = ["manual"],
    )
    _refused_test(
        name = name + "_refuses_a_copy_named_by_file",
        target_under_test = ":member_copy_file_checked",
    )
    pkg_files(
        name = "member_copy_files",
        srcs = [":member_copy.bin"],
        prefix = "switchroot/ios",
        tags = ["manual"],
    )
    pkg_zip(
        name = "member_copy_zip",
        srcs = [":member_copy_files"],
        tags = ["manual"],
    )
    ipsw_free(
        name = "member_copy_zip_checked",
        src = ":member_copy_zip",
        tags = ["manual"],
    )
    _refused_test(
        name = name + "_refuses_a_zip_holding_a_copy",
        target_under_test = ":member_copy_zip_checked",
    )
    _checked_copy(
        name = "validation_only",
        check = member,
        tags = ["manual"],
    )
    ipsw_free(
        name = "validation_only_checked",
        src = ":validation_only",
        tags = ["manual"],
    )
    _passed_test(
        name = name + "_passes_a_validation",
        target_under_test = ":validation_only_checked",
    )
    native.filegroup(
        name = "declassified_files",
        srcs = declassified,
        tags = ["manual"],
    )
    ipsw_free(
        name = "declassified_checked",
        src = ":declassified_files",
        tags = ["manual"],
    )
    _declassified_test(
        name = name + "_passes_the_declassified",
        target_under_test = ":declassified_checked",
    )
    ipsw_free(
        name = "window_checked",
        src = window,
        tags = ["manual"],
    )
    _window_refused_test(
        name = name + "_refuses_the_window",
        target_under_test = ":window_checked",
    )
    native.test_suite(
        name = name,
        tests = [
            name + "_refuses_a_copy",
            name + "_refuses_a_copy_named_by_file",
            name + "_refuses_a_zip_holding_a_copy",
            name + "_passes_a_validation",
            name + "_passes_the_declassified",
            name + "_refuses_the_window",
        ],
    )
