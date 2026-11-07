"""NVK's compiler identity (compiler_identity.py): the build ID NVK hashes into its pipeline cache UUID.

  mesa_identity_files   in each Mesa package: its source files, which compiler_identity.py chooses from
  nvk_compiler_identity nvk_build_id.h (NVK_BUILD_ID_OVERRIDE) for nvk_physical_device.c, and the ID as text
"""

load(":defs.bzl", "MESA")

def mesa_identity_files():  # buildifier: disable=unnamed-macro
    """Every source file of this package, as :identity_files (compiler_identity.py counts the compiler's)."""
    native.filegroup(
        name = "identity_files",
        srcs = native.glob(
            ["**"],
            exclude = [
                "**/.DS_Store",
                "**/__pycache__/**",
                "**/*.pyc",
            ],
        ),
        visibility = ["//third_party/mesa/src/nouveau/vulkan:__pkg__"],
    )

def _nvk_compiler_identity_impl(ctx):
    header = ctx.actions.declare_file(ctx.attr.header)
    text = ctx.actions.declare_file(ctx.attr.name + ".txt")
    args = ctx.actions.args()
    args.use_param_file("@%s", use_always = True)
    args.set_param_file_format("multiline")
    args.add("--mesa", MESA)
    args.add("--header", header)
    args.add("--text", text)
    args.add_all(ctx.files.srcs)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = ctx.files.srcs,
        outputs = [header, text],
        mnemonic = "NvkCompilerIdentity",
        progress_message = "Hashing NVK's compiler identity",
    )
    return [
        DefaultInfo(files = depset([header])),
        OutputGroupInfo(text = depset([text])),
    ]

nvk_compiler_identity = rule(
    implementation = _nvk_compiler_identity_impl,
    doc = "The header defining NVK_BUILD_ID_OVERRIDE (default files) and the ID alone (output group text).",
    attrs = {
        "header": attr.string(mandatory = True, doc = "the header's name, in this package's output directory"),
        "srcs": attr.label_list(mandatory = True, allow_files = True, doc = "every package's :identity_files"),
        "_tool": attr.label(default = "//third_party/mesa/bazel:compiler_identity", executable = True, cfg = "exec"),
    },
)
