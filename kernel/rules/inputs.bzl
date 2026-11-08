"""Inputs the kernel and its package derive from the IPSW and from pinned files: one rule runs a tool on declared
inputs into declared outputs."""

load(":common.bzl", "KERNEL_TOOL_ATTRS", "run_tool", "setting")

def _tool_output(ctx, xcode):
    if bool(ctx.attr.tool) == bool(ctx.attr.binary):
        fail("{}: give exactly one of tool and binary".format(ctx.label))
    if not xcode and [a for a in ctx.attr.args if "{xcode_env}" in a]:
        fail("{}: {{xcode_env}} needs xcode = True".format(ctx.label))
    outs = [ctx.actions.declare_file(o) for o in ctx.attr.outs] + [ctx.actions.declare_directory(d) for d in ctx.attr.out_dirs]
    args = []
    for a in ctx.attr.args:
        for i, _ in enumerate(ctx.attr.outs + ctx.attr.out_dirs):
            a = a.replace("{out%d}" % i, outs[i].path)
        a = a.replace("{out}", outs[0].path)
        if xcode:
            a = a.replace("{xcode_env}", ctx.file._xcode_env.path)
        a = a.replace("{path_root}", setting(ctx, "_path_root")).replace("{bin_dir}", ctx.bin_dir.path)
        args.append(ctx.expand_location(a, ctx.attr.srcs))
    run_tool(
        ctx,
        ctx.attr.binary or ctx.attr.tool,
        args,
        depset(ctx.files.srcs),
        outs,
        ctx.attr.mnemonic,
        ctx.attr.message or ("Making " + ", ".join(ctx.attr.outs + ctx.attr.out_dirs)),
        execution_requirements = {r: "" for r in ctx.attr.requirements} if ctx.attr.requirements else None,
        xcode = xcode,
    )
    return [DefaultInfo(files = depset(outs))]

def _tool_output_xcode_impl(ctx):
    return _tool_output(ctx, True)

def _tool_output_plain_impl(ctx):
    return _tool_output(ctx, False)

_ATTRS = {
    "tool": attr.string(doc = "a //tools/kernel tool name (kernel/rules/common.bzl KERNEL_TOOLS)"),
    "binary": attr.label(executable = True, cfg = "exec", doc = "or any executable target"),
    "srcs": attr.label_list(allow_files = True),
    "outs": attr.string_list(),
    "out_dirs": attr.string_list(),
    "args": attr.string_list(),
    "mnemonic": attr.string(default = "KernelTool"),
    "message": attr.string(),
    "requirements": attr.string_list(),
}

_DOC = ("Runs a tool (a //tools/kernel name, or any py_binary) with args ($(execpath), {out}/{outN}, {xcode_env}, " +
        "{path_root}, {bin_dir} expanded) on srcs into outs and out_dirs.")

_tool_output_xcode = rule(
    implementation = _tool_output_xcode_impl,
    doc = _DOC,
    attrs = dict(KERNEL_TOOL_ATTRS, **_ATTRS),
)

# Without the Xcode attributes at all: an output made without Xcode stays out of a graph that must hold none of it (the
# release image builder's on a Mac without Xcode, where @xcode's targets are stand-ins).
_tool_output_plain = rule(
    implementation = _tool_output_plain_impl,
    doc = _DOC,
    attrs = dict({k: v for k, v in KERNEL_TOOL_ATTRS.items() if k not in ("_xcode_env", "_xcode_identity")}, **_ATTRS),
)

def tool_output(name, xcode = True, **kwargs):
    """`xcode = False` for a tool that runs nothing of Xcode's: its action takes no Xcode input and runs without one."""
    (_tool_output_xcode if xcode else _tool_output_plain)(name = name, **kwargs)
