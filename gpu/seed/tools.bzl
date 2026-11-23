"""The shader seed's host tools: built here (seed_tools), or the release's prebuilt ones (a filegroup of the same
files below one directory), and the tar the release ships them as (tools_archive).

The seed's steps take only the tools' files they run (seed_tool_files): a raw pack generator edit changes the tools'
raw/ and MANIFEST.json, which no shader compile reads.
"""

_XCODE = {
    "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
    "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
}

_DARWIN = {"requires-darwin": ""}

# The raw sources' Python that the raw build step reads: make_pack.py imports packfile.py at start. Python finds a module
# beside a script through the script's real path, so an undeclared one still imported in the local sandbox (its inputs
# are links into the source tree) and failed on a remote worker.
_METAL_BUILD_MODULES = ["packfile.py", "selector_section.py"]

# What make_pack.py build makes (its PACK_TOOLS), each its own output: a step's action reads only the tools it runs
# (raw_tool_files), so a tool it does not run reruns none of it.
_PACK_TOOLS = ["directpack", "native-seed", "libmock_iokit.dylib", "own.metallib", "context.log"]

SeedToolsInfo = provider(
    doc = "The shader tools' files, each on its own: what a seed step runs, without the rest of the tools.",
    fields = {
        "manifest": "MANIFEST.json (the stack key)",
        "raw": "the pack's tools, one file each (_PACK_TOOLS)",
        "raw_dir": "the directory holding them",
    },
)

def root_of(files, marker):
    """The directory holding `marker` among `files`: a tree artifact, or a filegroup's files below one directory."""
    for f in files:
        if f.basename == marker:
            return f.dirname
    trees = [f for f in files if f.is_directory]
    if len(trees) != 1:
        fail("expected one directory, or files including {}".format(marker))
    return trees[0].path

def seed_tool_files(target):
    """SeedToolsInfo of the local tools, or of the release's (a filegroup with MANIFEST.json at its root)."""
    if SeedToolsInfo in target:
        return target[SeedToolsInfo]
    files = target[DefaultInfo].files.to_list()
    root = root_of(files, "MANIFEST.json")
    manifest = [f for f in files if f.dirname == root and f.basename == "MANIFEST.json"]
    if not manifest:
        fail("the shader tools have no MANIFEST.json")
    return SeedToolsInfo(
        manifest = manifest[0],
        raw = [f for f in files if f.dirname == root + "/raw"],
        raw_dir = root + "/raw",
    )

def raw_tool_files(tools, names):
    """The files of SeedToolsInfo `tools`' raw tools named `names`; fails when one is missing."""
    found = {f.basename: f for f in tools.raw if f.basename in names}
    missing = [n for n in names if n not in found]
    if missing:
        fail("the raw tools have no " + ", ".join(missing))
    return [found[n] for n in names]

def _pack_build(ctx, name):
    """The pack generator's tools (pack/make_pack.py build), one file each."""
    tree = ctx.file.gm20b_generator.dirname.partition("drivers/TegraGM20BMetal/")[0].rstrip("/") or "."
    raw = [ctx.actions.declare_file(name + "/" + tool) for tool in _PACK_TOOLS]
    raw_dir = raw[0].dirname
    args = ctx.actions.args()
    args.add("build")
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--translate", ctx.file.translate)
    args.add("--compile", ctx.file.compile)
    args.add("--metal-abi", ctx.file.metal_abi)
    args.add("--context-header", ctx.file.context_header)
    args.add("--generator", ctx.file.gm20b_generator)
    args.add("--tree", tree)
    args.add("--out", raw_dir)
    sources = [f for f in ctx.files.gm20b_srcs if f.extension != "py" or f.basename in _METAL_BUILD_MODULES]
    ctx.actions.run(
        executable = ctx.executable._make_pack,
        arguments = [args],
        inputs = depset(
            [ctx.file.translate, ctx.file.compile, ctx.file.metal_abi, ctx.file.context_header, ctx.file.gm20b_generator] +
            [ctx.file._xcode_env, ctx.file._xcode_identity] + sources,
        ),
        outputs = raw,
        mnemonic = "SeedToolsRaw",
        progress_message = "Building the pack's tools and mock GM20B",
        execution_requirements = _DARWIN,
    )
    return raw, raw_dir

def _seed_tools_impl(ctx):
    raw, raw_dir = _pack_build(ctx, ctx.attr.name + "_raw")
    out = ctx.actions.declare_directory(ctx.attr.name)
    manifest = ctx.actions.declare_file(ctx.attr.name + "_manifest/MANIFEST.json")
    args = ctx.actions.args()
    args.add("layout")
    args.add("--raw", raw_dir)
    args.add("--stack", ctx.file.stack)
    args.add("--out", out.path)
    args.add("--manifest", manifest)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [ctx.file.stack] + raw,
        outputs = [out, manifest],
        mnemonic = "SeedToolsLayout",
        progress_message = "Laying out the shader tools",
    )
    return [
        DefaultInfo(files = depset([out])),
        SeedToolsInfo(manifest = manifest, raw = raw, raw_dir = raw_dir),
    ]

seed_tools = rule(
    implementation = _seed_tools_impl,
    doc = """The host tools the seed runs (directpack and native-seed over the translation and the stage compile, the mock
    GM20B, own.metallib, the context init) and MANIFEST.json (the stack key), as one directory: the release ships it
    prebuilt.""",
    attrs = _XCODE | {
        "compile": attr.label(mandatory = True, allow_single_file = True, doc = "the stage compile's macOS object"),
        "context_header": attr.label(mandatory = True, allow_single_file = True, doc = "the channel's context init"),
        "metal_abi": attr.label(mandatory = True, allow_single_file = True, doc = "the selected IPSW kind's MetalABI.h"),
        "gm20b_generator": attr.label(mandatory = True, allow_single_file = True, doc = "pack/make_pack.py"),
        "gm20b_srcs": attr.label_list(allow_files = True),
        "stack": attr.label(mandatory = True, allow_single_file = True, doc = "the driver's stack key"),
        "translate": attr.label(mandatory = True, allow_single_file = True, doc = "the translation alone"),
        "_tool": attr.label(default = "//tools/gpu:seed", executable = True, cfg = "exec"),
        "_make_pack": attr.label(default = "//tools/gpu:make_pack", executable = True, cfg = "exec"),
    },
)

def _tools_archive_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.name + ".tar")
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = ["archive", "--tools", root_of(ctx.files.tools, "MANIFEST.json"), "--out", out.path],
        inputs = ctx.files.tools,
        outputs = [out],
        mnemonic = "SeedToolsArchive",
        progress_message = "Archiving the shader tools",
    )
    return [DefaultInfo(files = depset([out]))]

tools_archive = rule(
    implementation = _tools_archive_impl,
    doc = "The tools directory as a tar (sorted, no times or owners): the release asset.",
    attrs = {
        "tools": attr.label(mandatory = True),
        "_tool": attr.label(default = "//tools/gpu:seed", executable = True, cfg = "exec"),
    },
)
