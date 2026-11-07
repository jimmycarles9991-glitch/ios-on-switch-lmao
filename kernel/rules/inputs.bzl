"""Inputs the kernel and its package derive from the IPSW and from pinned files: one rule runs a tool on declared
inputs into declared outputs."""

load(":common.bzl", "KERNEL_TOOL_ATTRS", "run_tool", "setting")

def _tool_output_impl(ctx):
    if bool(ctx.attr.tool) == bool(ctx.attr.binary):
        fail("{}: give exactly one of tool and binary".format(ctx.label))
    if not ctx.attr.xcode and [a for a in ctx.attr.args if "{xcode_env}" in a]:
        fail("{}: {{xcode_env}} needs xcode = True".format(ctx.label))
    outs = [ctx.actions.declare_file(o) for o in ctx.attr.outs] + [ctx.actions.declare_directory(d) for d in ctx.attr.out_dirs]
    args = []
    for a in ctx.attr.args:
        for i, _ in enumerate(ctx.attr.outs + ctx.attr.out_dirs):
            a = a.replace("{out%d}" % i, outs[i].path)
        a = a.replace("{out}", outs[0].path)
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
        xcode = ctx.attr.xcode,
    )
    return [DefaultInfo(files = depset(outs))]

tool_output = rule(
    implementation = _tool_output_impl,
    doc = "Runs a tool (a //tools/kernel name, or any py_binary) with args ($(execpath), {out}/{outN}, {xcode_env}, " +
          "{path_root}, {bin_dir} expanded) on srcs into outs and out_dirs.",
    attrs = dict(KERNEL_TOOL_ATTRS, **{
        "tool": attr.string(doc = "a //tools/kernel tool name (kernel/rules/common.bzl KERNEL_TOOLS)"),
        "binary": attr.label(executable = True, cfg = "exec", doc = "or any executable target"),
        "srcs": attr.label_list(allow_files = True),
        "outs": attr.string_list(),
        "out_dirs": attr.string_list(),
        "args": attr.string_list(),
        "mnemonic": attr.string(default = "KernelTool"),
        "message": attr.string(),
        "requirements": attr.string_list(),
        "xcode": attr.bool(default = True, doc = "False for a tool that runs nothing of Xcode's (no Xcode input)"),
    }),
)
