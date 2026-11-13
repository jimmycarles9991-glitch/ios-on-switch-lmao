"""The userland overlay's iOS binaries (ios_binary). The boot entry itself is //boot:package (boot/defs.bzl)."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//bazel:profile.bzl", "switch_debug_define")
load("//translate/arm64e:target.bzl", "a64x_target_linkopts")
load(":common.bzl", "KERNEL_TOOL_ATTRS", "PY_TOOLCHAIN", "run_tool", "setting")

def _ios_binary_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.out)
    args = [
        "--xcode-env",
        ctx.file._xcode_env.path,
        "--out",
        out.path,
        "--path-root",
        setting(ctx, "_path_root"),
        "--bin-dir",
        ctx.bin_dir.path,
    ]
    if ctx.attr.coreaudio_headers:
        args.append("--coreaudio-headers")
    if ctx.attr.sign_identifier:
        args += ["--sign-identifier", ctx.attr.sign_identifier]
    if ctx.file.entitlements:
        args += ["--entitlements", ctx.file.entitlements.path]
    if ctx.attr.pagesize:
        args += ["--pagesize", ctx.attr.pagesize]
    if ctx.attr._profile[BuildSettingInfo].value == "release":
        # Stripped before the signature (the release profile), so the CDHash the trust cache holds is the shipped one.
        args.append("--strip")
    args += ["--", switch_debug_define(ctx)]
    for flag in ctx.attr.copts:
        args.append(flag.replace("{tree}", ctx.label.package).replace("{bin}", ctx.bin_dir.path))
    args += ctx.attr.linkopts
    args += [f.path for f in ctx.files.srcs]
    run_tool(
        ctx,
        "ios_binary.py",
        args,
        depset(ctx.files.srcs + ctx.files.hdrs + ([ctx.file.entitlements] if ctx.file.entitlements else [])),
        [out],
        "IosBinary",
        "Building " + ctx.attr.out,
    )
    return [DefaultInfo(files = depset([out]))]

_ios_binary = rule(
    implementation = _ios_binary_impl,
    doc = "An ad hoc signed arm64 iOS 17 binary (the overlay's tools, plug-ins and decoders).",
    attrs = dict(KERNEL_TOOL_ATTRS, **{
        "out": attr.string(mandatory = True),
        "srcs": attr.label_list(allow_files = True, mandatory = True),
        "hdrs": attr.label_list(allow_files = True),
        "copts": attr.string_list(),
        "linkopts": attr.string_list(),
        "coreaudio_headers": attr.bool(),
        "sign_identifier": attr.string(),
        "entitlements": attr.label(allow_single_file = True),
        "pagesize": attr.string(),
        "_profile": attr.label(default = "//bazel:profile"),
    }),
    toolchains = [PY_TOOLCHAIN],
)

def ios_binary(name, **kwargs):
    """An ad hoc signed arm64 iOS binary, marked as built for this core where the kernel reads the marker.

    Args:
        name: the target's name.
        **kwargs: _ios_binary's attributes.
    """
    _ios_binary(name = name, linkopts = a64x_target_linkopts(), **kwargs)
