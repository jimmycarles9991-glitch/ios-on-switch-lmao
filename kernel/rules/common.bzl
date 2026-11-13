"""Shared plumbing for the kernel rules: the tools (py_binary targets on the hermetic Python), the Xcode host input,
the path settings that keep sandbox paths out of the outputs."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

PY_TOOLCHAIN = "@rules_python//python:toolchain_type"

# Every action that runs an Xcode tool: macOS only, here or on a Mac remote worker with the same Xcode at the same path
# (the exec platform's "xcode" property; the action key includes @xcode//:identity).
XCODE_EXECUTION = {"requires-darwin": ""}

def python(ctx):
    runtime = ctx.toolchains[PY_TOOLCHAIN].py3_runtime
    return struct(interpreter = runtime.interpreter, files = runtime.files)

def setting(ctx, attr):
    return getattr(ctx.attr, attr)[BuildSettingInfo].value

def workspace_root(target):
    """`external/<canonical repo name>` of a target in an external repository ("" for the main repository)."""
    return target.label.workspace_root

# The kernel build's tools: py_binary targets in tools/kernel (each on the hermetic rules_python toolchain), named by
# their script's stem. run_tool(ctx, "corecrypto", ...) runs //tools/kernel:corecrypto.
KERNEL_TOOLS = [
    "corecrypto",
    "data_seed",
    "firehose",
    "host_c_binary",
    "im4p",
    "ios_binary",
    "libpthread",
    "storage_abi",
    "unxz",
    "xnu_symbols",
]

KERNEL_TOOL_ATTRS = {
    "_tools": attr.label_list(default = ["//tools/kernel:" + t for t in KERNEL_TOOLS], cfg = "exec"),
    "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
    "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
    "_path_root": attr.label(default = "//kernel/rules:path_root"),
    "_build_date": attr.label(default = "//kernel/rules:build_date"),
    "_builder": attr.label(default = "//kernel/rules:builder"),
}

def run_tool(ctx, tool, arguments, inputs, outputs, mnemonic, progress, execution_requirements = None, env = None, xcode = True):
    """Runs a tool: a tools/kernel name ("corecrypto", or "corecrypto.py") or any executable target (a py_binary).

    xcode = False for a tool that runs nothing of Xcode's: its action then takes no Xcode input and runs without one.
    """
    if type(tool) == "string":
        name = tool[:-3] if tool.endswith(".py") else tool
        found = [t for t in ctx.attr._tools if t.label.name == name]
        if len(found) != 1:
            fail("no //tools/kernel:" + name)
        tool = found[0]
    ctx.actions.run(
        executable = tool[DefaultInfo].files_to_run,
        arguments = arguments,
        inputs = depset([ctx.file._xcode_env, ctx.file._xcode_identity], transitive = [inputs]) if xcode else inputs,
        outputs = outputs,
        mnemonic = mnemonic,
        progress_message = progress,
        execution_requirements = execution_requirements or (XCODE_EXECUTION if xcode else {}),
        env = env or {},
    )
