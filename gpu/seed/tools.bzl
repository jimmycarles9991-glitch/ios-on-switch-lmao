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

# The raw sources' Python that the raw build step reads: rawpack.py imports packfile.py at start. Python finds a module
# beside a script through the script's real path, so an undeclared one still imported in the local sandbox (its inputs
# are links into the source tree) and failed on a remote worker.
_RAW_BUILD_MODULES = ["packfile.py", "selector_section.py"]

# What rawpack.py build makes (its PACK_TOOLS, and BUILT with the host bridge), each its own output: a step's action reads
# only the tools it runs (raw_tool_files), so a tool it does not run reruns none of it.
_PACK_TOOLS = ["directpack", "libmock_iokit.dylib", "own.metallib", "context.log"]
_BRIDGE_TOOLS = ["probe", "native-seed", "compile-profile"]

SeedToolsInfo = provider(
    doc = "The shader tools' files, each on its own: what a seed step runs, without the rest of the tools.",
    fields = {
        "precompile": "the precompile tool",
        "mock": "libmock_iokit.dylib (the tool's @rpath)",
        "twin": "the NVK twin, libvulkan_nouveau.dylib",
        "manifest": "MANIFEST.json (the stack key, the twin's LC_UUID and compiler identity)",
        "nvk_identity": "the twin's .source (its Mesa tree and compiler identity), or None: what a bucket is keyed on",
        "raw": "the raw pack's tools, one file each (_PACK_TOOLS; with the bridge also _BRIDGE_TOOLS)",
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
    top = {f.basename: f for f in files if f.dirname == root}
    for name in ("precompile", "libmock_iokit.dylib", "libvulkan_nouveau.dylib", "MANIFEST.json"):
        if name not in top:
            fail("the shader tools have no {} beside MANIFEST.json".format(name))
    return SeedToolsInfo(
        precompile = top["precompile"],
        mock = top["libmock_iokit.dylib"],
        twin = top["libvulkan_nouveau.dylib"],
        manifest = top["MANIFEST.json"],
        nvk_identity = top.get("libvulkan_nouveau.dylib.source"),
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

def _raw_build(ctx, name, host_bridge):
    """The pack generator's tools (raw/pack/rawpack.py build), one file each; with `host_bridge` the frontend's too."""
    tree = ctx.file.raw_generator.dirname.partition("drivers/TegraGM20BMetal/")[0].rstrip("/") or "."
    raw = [ctx.actions.declare_file(name + "/" + tool) for tool in _PACK_TOOLS + (_BRIDGE_TOOLS if host_bridge else [])]
    raw_dir = raw[0].dirname
    args = ctx.actions.args()
    args.add("build")
    args.add("--xcode-env", ctx.file._xcode_env)
    if host_bridge:
        args.add("--host-bridge", host_bridge)
    args.add("--translate", ctx.file.translate)
    args.add("--compile", ctx.file.compile)
    args.add("--metal-abi", ctx.file.metal_abi)
    args.add("--context-header", ctx.file.context_header)
    args.add("--generator", ctx.file.raw_generator)
    args.add("--tree", tree)
    args.add("--out", raw_dir)
    sources = [f for f in ctx.files.raw_srcs if f.extension != "py" or f.basename in _RAW_BUILD_MODULES]
    ctx.actions.run(
        executable = ctx.executable._rawpack,
        arguments = [args],
        inputs = depset(
            [ctx.file.translate, ctx.file.compile, ctx.file.metal_abi, ctx.file.context_header, ctx.file.raw_generator] +
            ([host_bridge] if host_bridge else []) + [ctx.file._xcode_env, ctx.file._xcode_identity] + sources,
        ),
        outputs = raw,
        mnemonic = "SeedToolsRaw",
        progress_message = "Building the raw pack's tools and mock GM20B",
        execution_requirements = _DARWIN,
    )
    return raw, raw_dir

def _seed_tools_impl(ctx):
    xcode = [ctx.file._xcode_env, ctx.file._xcode_identity]
    tool = ctx.executable._tool
    steps = {}
    for step, inputs, extra in (
        ("mock", [ctx.file.abi_header, ctx.file.mock_iokit], ["--abi-header", ctx.file.abi_header, "--mock-iokit", ctx.file.mock_iokit]),
        ("twin", [ctx.file.nvk, ctx.file.twin] + ctx.files.nvk_sources, ["--nvk", ctx.file.nvk, "--twin", ctx.file.twin]),
    ):
        out = ctx.actions.declare_file("{}_{}/{}".format(ctx.attr.name, step, "libmock_iokit.dylib" if step == "mock" else "libvulkan_nouveau.dylib"))
        args = ctx.actions.args()
        args.add(step)
        if step == "mock":
            args.add("--xcode-env", ctx.file._xcode_env)
        args.add_all(extra)
        args.add("--out", out)
        ctx.actions.run(
            executable = tool,
            arguments = [args],
            inputs = inputs + (xcode if step == "mock" else []),
            outputs = [out],
            mnemonic = "SeedTools" + step.capitalize(),
            progress_message = "Building the shader tools' " + step,
            execution_requirements = _DARWIN,
        )
        steps[step] = out

    precompile = ctx.actions.declare_file(ctx.attr.name + "_tool/precompile")
    args = ctx.actions.args()
    args.add("tool")
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--metal-header", ctx.file.metal_header)
    args.add("--metal-abi", ctx.file.metal_abi)
    args.add("--precompile", ctx.file.precompile)
    args.add("--frontend", ctx.file.frontend)
    args.add("--host-bridge", ctx.file.host_bridge)
    args.add("--mock", steps["mock"])
    args.add("--out", precompile)
    ctx.actions.run(
        executable = tool,
        arguments = [args],
        inputs = [ctx.file.metal_header, ctx.file.metal_abi, ctx.file.precompile, ctx.file.frontend, ctx.file.host_bridge, steps["mock"]] +
                 ctx.files.hdrs + xcode,
        outputs = [precompile],
        mnemonic = "SeedToolsPrecompile",
        progress_message = "Building the shader cache precompile tool",
        execution_requirements = _DARWIN,
    )

    raw, raw_dir = _raw_build(ctx, ctx.attr.name + "_raw", ctx.file.host_bridge)

    out = ctx.actions.declare_directory(ctx.attr.name)
    manifest = ctx.actions.declare_file(ctx.attr.name + "_manifest/MANIFEST.json")
    nvk_source = [f for f in ctx.files.nvk_sources if f.basename.endswith(".source")][0]
    args = ctx.actions.args()
    args.add("layout")
    args.add("--precompile", precompile)
    args.add("--mock", steps["mock"])
    args.add("--twin-dylib", steps["twin"])
    args.add("--raw", raw_dir)
    args.add("--stack", ctx.file.stack)
    args.add("--nvk-source", nvk_source)
    args.add("--out", out.path)
    args.add("--manifest", manifest)
    ctx.actions.run(
        executable = tool,
        arguments = [args],
        inputs = [precompile, steps["mock"], steps["twin"], ctx.file.stack] + raw + ctx.files.nvk_sources,
        outputs = [out, manifest],
        mnemonic = "SeedToolsLayout",
        progress_message = "Laying out the shader tools",
    )
    return [
        DefaultInfo(files = depset([out])),
        SeedToolsInfo(
            precompile = precompile,
            mock = steps["mock"],
            twin = steps["twin"],
            manifest = manifest,
            nvk_identity = nvk_source,
            raw = raw,
            raw_dir = raw_dir,
        ),
    ]

seed_tools = rule(
    implementation = _seed_tools_impl,
    doc = "The host tools the seed runs, as one directory (the release ships it prebuilt).",
    attrs = _XCODE | {
        "abi_header": attr.label(mandatory = True, allow_single_file = True),
        "compile": attr.label(mandatory = True, allow_single_file = True, doc = "the stage compile's macOS object"),
        "mock_iokit": attr.label(mandatory = True, allow_single_file = True),
        "metal_header": attr.label(mandatory = True, allow_single_file = True),
        "metal_abi": attr.label(mandatory = True, allow_single_file = True, doc = "the selected IPSW kind's MetalABI.h"),
        "context_header": attr.label(mandatory = True, allow_single_file = True, doc = "the channel's context init"),
        "precompile": attr.label(mandatory = True, allow_single_file = True),
        "frontend": attr.label(mandatory = True, allow_single_file = True),
        "hdrs": attr.label_list(allow_files = True),
        "host_bridge": attr.label(mandatory = True, allow_single_file = True),
        "nvk": attr.label(mandatory = True, allow_single_file = True, doc = "the iOS dylib, whose identity the twin takes"),
        "twin": attr.label(mandatory = True, allow_single_file = True),
        "nvk_sources": attr.label_list(allow_files = True, doc = "the two dylibs' .source files"),
        "raw_generator": attr.label(mandatory = True, allow_single_file = True, doc = "raw/pack/rawpack.py"),
        "raw_srcs": attr.label_list(allow_files = True),
        "stack": attr.label(mandatory = True, allow_single_file = True, doc = "the raw path's stack key"),
        "translate": attr.label(mandatory = True, allow_single_file = True, doc = "the translation alone (no m2v runtime)"),
        "_tool": attr.label(default = "//tools/gpu:seed", executable = True, cfg = "exec"),
        "_rawpack": attr.label(default = "//tools/gpu:rawpack", executable = True, cfg = "exec"),
    },
)

def _raw_tools_impl(ctx):
    raw, raw_dir = _raw_build(ctx, ctx.attr.name + "_raw", None)
    manifest = ctx.actions.declare_file(ctx.attr.name + "_manifest/MANIFEST.json")
    ctx.actions.run_shell(
        command = 'printf \'{\\n "format": 1,\\n "stack": "%s"\\n}\\n\' "$(cat "$1")" > "$2"',
        arguments = [ctx.file.stack.path, manifest.path],
        inputs = [ctx.file.stack],
        outputs = [manifest],
        mnemonic = "RawToolsManifest",
        progress_message = "Writing the raw pack tools' manifest",
    )
    return [
        DefaultInfo(files = depset(raw + [manifest])),
        SeedToolsInfo(
            precompile = None,
            mock = None,
            twin = None,
            manifest = manifest,
            nvk_identity = None,
            raw = raw,
            raw_dir = raw_dir,
        ),
    ]

raw_tools = rule(
    implementation = _raw_tools_impl,
    doc = """The raw pack's own tools (directpack over the translation and the stage compile, the mock GM20B, own.metallib,
    the context init) and a manifest of the stack key alone: what the raw pack is made with, no m2v in them.""",
    attrs = _XCODE | {
        "compile": attr.label(mandatory = True, allow_single_file = True, doc = "the stage compile's macOS object"),
        "context_header": attr.label(mandatory = True, allow_single_file = True, doc = "the channel's context init"),
        "metal_abi": attr.label(mandatory = True, allow_single_file = True, doc = "the selected IPSW kind's MetalABI.h"),
        "raw_generator": attr.label(mandatory = True, allow_single_file = True, doc = "raw/pack/rawpack.py"),
        "raw_srcs": attr.label_list(allow_files = True),
        "stack": attr.label(mandatory = True, allow_single_file = True, doc = "the raw path's stack key"),
        "translate": attr.label(mandatory = True, allow_single_file = True, doc = "the translation alone (no m2v runtime)"),
        "_rawpack": attr.label(default = "//tools/gpu:rawpack", executable = True, cfg = "exec"),
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
