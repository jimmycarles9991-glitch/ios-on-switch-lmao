"""directrun and exec.metallib (direct/build.py), built beside the shader tools' mock GM20B."""

load("//gpu/seed:tools.bzl", "SeedToolsInfo")

def _direct_runner_impl(ctx):
    out = ctx.actions.declare_directory(ctx.attr.name)
    tools = ctx.attr.tools[SeedToolsInfo]
    args = ctx.actions.args()
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--translate", ctx.file.translate)
    args.add("--compile", ctx.file.compile)
    args.add("--metal-abi", ctx.file.metal_abi)
    args.add("--raw", tools.raw_dir)
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._build,
        arguments = [args],
        inputs = depset(
            ctx.files.srcs + tools.raw +
            [ctx.file.translate, ctx.file.compile, ctx.file.metal_abi, ctx.file._xcode_env, ctx.file._xcode_identity],
        ),
        outputs = [out],
        mnemonic = "DirectRunner",
        progress_message = "Building the direct compile's runner",
        execution_requirements = {"requires-darwin": ""},
    )
    return [DefaultInfo(files = depset([out]), runfiles = ctx.runfiles([out]))]

direct_runner = rule(
    implementation = _direct_runner_impl,
    doc = "directrun, its mock GM20B and exec.metallib, as one directory.",
    attrs = {
        "srcs": attr.label_list(allow_files = True, doc = "directrun.mm, exec.metal and every source they compile"),
        "translate": attr.label(mandatory = True, allow_single_file = True),
        "compile": attr.label(
            default = "//third_party/mesa/src/nouveau/switch_compile:host",
            allow_single_file = True,
            doc = "the stage compile's macOS object",
        ),
        "metal_abi": attr.label(mandatory = True, allow_single_file = True),
        "tools": attr.label(mandatory = True, providers = [SeedToolsInfo]),
        "_build": attr.label(default = "//tools/gpu/maxwell:direct_build", executable = True, cfg = "exec"),
        "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
        "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
    },
)
