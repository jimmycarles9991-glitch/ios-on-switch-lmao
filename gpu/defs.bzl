"""Rules for the TegraGM20B Metal bundle (gpu/BUILD.bazel). Each step of the old build-tegra-gm20b-metal.py is one
action with declared inputs, so Bazel reruns only what an edit reaches:

  llvm-as (2 fixtures) -> rustc per crate (iOS bridge: bazel/rust) -> clang++ (each object) -> link -> strip/sign/trust cache

The shader seed and the raw pack are gpu/seed's.
"""

load("//bazel:profile.bzl", "PROFILE_ATTR", "switch_debug_define")
load("//gpu/seed:defs.bzl", "SeedInfo")
load("//ipsw:ipsw.bzl", "IpswInfo")
load("//kernel/rules:layout.bzl", "MOVED_ROOTS", "OLD_TREE", "TREE_ROOT")

_XCODE = {
    "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
    "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
}

# --- llvm-as ---------------------------------------------------------------------------------------------------------

def _llvm_as_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.out)
    ctx.actions.run(
        executable = ctx.executable._llvm_as,
        arguments = ["-o", out.path, ctx.file.src.path],
        inputs = [ctx.file.src],
        outputs = [out],
        mnemonic = "LlvmAs",
        progress_message = "Assembling %{input}",
    )
    return [DefaultInfo(files = depset([out]))]

llvm_as = rule(
    implementation = _llvm_as_impl,
    attrs = {
        "src": attr.label(mandatory = True, allow_single_file = [".ll"]),
        "out": attr.string(mandatory = True),
        "_llvm_as": attr.label(default = "@gpu_llvm_as//:bin/llvm-as", executable = True, cfg = "exec", allow_single_file = True),
    },
)

# --- Xcode clang: objects and the plugin link --------------------------------------------------------------------

def _xcode_run(ctx, mnemonic, message, command, inputs, outputs, env = {}):
    args = ctx.actions.args()
    args.add(ctx.file._xcode_env)
    args.add_all(["%s=%s" % (k, v) for k, v in env.items()])
    args.add("--")
    args.add_all(command)
    ctx.actions.run(
        executable = ctx.executable._xcode_run,
        arguments = [args],
        inputs = depset(inputs + [ctx.file._xcode_env, ctx.file._xcode_identity]),
        outputs = outputs,
        mnemonic = mnemonic,
        progress_message = message,
    )

_IOS = ["-target", "arm64-apple-ios17.0", "-isysroot", "@IPHONEOS_SDK@"]

# The old script's one clang++ line, split into compile and link so each object is its own cached action. Paths in
# debug info and __FILE__ are mapped to /switch-ios, and the link keeps no object paths (oso_prefix) or times.
# -fno-objc-msgsend-selector-stubs: ld's own objc_msgSend selector stubs (its default "fast" __objc_stubs, which the
# old build had) come out in a different order in about half of all links (measured: 2 outputs in 10 links), so the
# plugin's CDHash, and with it the trust cache and the kernel window, changed on every build. Classic call sites
# (selector loaded inline, objc_msgSend through __stubs) take the same instructions per message and link identically
# (10 of 10), at +10 KB of __text.
_PLUGIN_CFLAGS = [
    "-O2",
    "-g",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-fstack-protector-strong",
    "-std=c++17",
    "-fobjc-arc",
    "-Wno-deprecated-declarations",
    "-fno-objc-msgsend-selector-stubs",
    "-ffile-prefix-map=@PWD@=/switch-ios",
] + [
    # The layout move's old spellings (kernel/rules/layout.bzl): the plugin's strings name its sources as before.
    "-ffile-prefix-map=@PWD@/{r}=/switch-ios/{o}/{r}".format(r = r, o = OLD_TREE)
    for r in MOVED_ROOTS
    if TREE_ROOT != OLD_TREE
] + [
    # The sources are compiled by their execution-root-relative path (src.path), which the line tables' directories
    # and __FILE__ keep relative: those get the old spelling too.
    "-ffile-prefix-map={r}={o}/{r}".format(r = r, o = OLD_TREE)
    for r in MOVED_ROOTS
    if TREE_ROOT != OLD_TREE
]

def _metal_plugin_impl(ctx):
    # -I in the old build's order (NativeMetal, raw/runtime, TegraGM20B), and the raw path's stack key as a define.
    includes = []
    for marker in ctx.files.include_markers:
        includes += ["-I", marker.dirname]
    defines = [switch_debug_define(ctx)]
    if ctx.file.stack:
        defines.append("-DTEGRA_RAW_STACK=0x@FILE:%s@ull" % ctx.file.stack.path)
    headers = ctx.files.hdrs + ctx.files.include_markers + ctx.files.stack
    objects = []
    for language, sources in (("objective-c++", ctx.files.srcs), ("c++", ctx.files.cc_srcs), ("c", ctx.files.c_srcs)):
        # Plain C++ sends no Objective-C messages (clang calls the flag unused there, an error under -Werror); C takes
        # neither the C++ standard nor ARC.
        cflags = [f for f in _PLUGIN_CFLAGS if language == "objective-c++" or f != "-fno-objc-msgsend-selector-stubs"]
        compiler = "clang++"
        if language == "c":
            # The C units are kits/SwitchGM20BEmit's emitters: their asserts are Mesa's debug checks, off as in Mesa's
            # release builds rather than new ways for a render server to abort.
            cflags = [f for f in cflags if f not in ("-std=c++17", "-fobjc-arc")] + ["-std=c11", "-DNDEBUG"]
            compiler = "clang"
        for src in sources:
            obj = ctx.actions.declare_file("%s_objs/%s.o" % (ctx.attr.name, src.basename))
            _xcode_run(
                ctx,
                "ObjcxxCompile",
                "Compiling %s" % src.short_path,
                [compiler] + _IOS + cflags + includes + defines + ["-x", language, "-c", src.path, "-o", obj.path],
                [src] + headers,
                [obj],
            )
            objects.append(obj)
    out = ctx.actions.declare_file(ctx.attr.name + "/TegraGM20BMetal")
    frameworks = []
    for framework in ctx.attr.frameworks:
        frameworks += ["-framework", framework]
    _xcode_run(
        ctx,
        "MetalPluginLink",
        "Linking the TegraGM20BMetal plugin",
        # The compile flags again, as the old one-line build had them: clang passes -O2 on to ld, whose code
        # deduplication it turns on (118 KB less __text).
        ["clang++"] + _IOS + _PLUGIN_CFLAGS + ["-bundle"] + [o.path for o in objects] + [ctx.file.bridge.path] +
        ["-Wl,-dead_strip", "-Wl,-oso_prefix,@PWD@/", "-lobjc"] + frameworks + ctx.attr.linkopts + ["-o", out.path],
        objects + [ctx.file.bridge],
        [out],
    )
    dsym = ctx.actions.declare_directory(ctx.attr.name + "/TegraGM20BMetal.dSYM")
    _xcode_run(
        ctx,
        "Dsymutil",
        "Writing the plugin's dSYM",
        ["dsymutil", "--oso-prepend-path=@PWD@", out.path, "-o", dsym.path],
        objects + [ctx.file.bridge, out],
        [dsym],
    )
    return [
        DefaultInfo(files = depset([out])),
        OutputGroupInfo(debug = depset([dsym]), objects = depset(objects)),
    ]

metal_plugin = rule(
    implementation = _metal_plugin_impl,
    attrs = _XCODE | PROFILE_ATTR | {
        "srcs": attr.label_list(mandatory = True, allow_files = [".mm"], doc = "Objective-C++ units, in link order"),
        "cc_srcs": attr.label_list(allow_files = [".cpp"], doc = "C++ units, linked after srcs"),
        "c_srcs": attr.label_list(allow_files = [".c"], doc = "C units, linked after cc_srcs"),
        "hdrs": attr.label_list(allow_files = True),
        "include_markers": attr.label_list(allow_files = True, doc = "one file per -I directory, in order"),
        "stack": attr.label(allow_single_file = True, doc = "the raw path's stack key (TEGRA_RAW_STACK)"),
        "frameworks": attr.string_list(default = ["Foundation", "Metal", "IOSurface"]),
        "linkopts": attr.string_list(doc = "added to the link"),
        "bridge": attr.label(mandatory = True, allow_single_file = True),
        "_xcode_run": attr.label(default = "//tools/gpu:xcode_run", executable = True, cfg = "exec"),
    },
)

# --- The raw GM20B path's stack key --------------------------------------------------------------------------------

_CrateSourcesInfo = provider(
    doc = "What a crate graph is compiled from: each crate's and build script's sources and compile data.",
    fields = {"files": "depset of File"},
)

_CRATE_EDGES = ("deps", "proc_macro_deps", "script")

def _crate_sources_impl(_target, ctx):
    files = []
    for name in ("srcs", "compile_data", "data"):
        files += getattr(ctx.rule.files, name, [])
    transitive = []
    for name in _CRATE_EDGES:
        edge = getattr(ctx.rule.attr, name, None)
        for dep in (edge if type(edge) == "list" else [edge] if edge else []):
            if _CrateSourcesInfo in dep:
                transitive.append(dep[_CrateSourcesInfo].files)
    return [_CrateSourcesInfo(files = depset(files, transitive = transitive))]

_crate_sources = aspect(
    implementation = _crate_sources_impl,
    attr_aspects = list(_CRATE_EDGES),
)

# A file by its path below the tree, never File.path: a generated file's names its configuration's output directory.
def _hashed_as(f):
    return "{}={}".format(f.short_path, f.path)

def _short_path(f):
    return f.short_path

# The preprocessor of each kind of unit, as the shader tools compile it (raw/pack/rawpack.py build)
_UNITS = {
    "c": ["clang", "-isysroot", "@MACOSX_SDK@", "-x", "c", "-std=c11"],
    "cpp": ["clang++", "-isysroot", "@MACOSX_SDK@", "-x", "c++", "-std=c++17"],
    "mm": ["clang++", "-isysroot", "@MACOSX_SDK@", "-x", "objective-c++", "-std=c++17", "-fobjc-arc"],
}

# Each unit's includes with the hashed files its only inputs: a header outside them is not found and the build fails.
def _includes_checked(ctx):
    headers = ctx.files.srcs + [ctx.file.metal_abi]
    includes = []
    for marker in ctx.files.include_markers + [ctx.file.metal_abi]:
        includes += ["-I", marker.dirname]
    checked = []
    for src in ctx.files.srcs:
        if src.extension not in _UNITS:
            continue
        out = ctx.actions.declare_file("{}_includes/{}.d".format(ctx.attr.name, src.short_path))
        _xcode_run(
            ctx,
            "RawStackIncludes",
            "Checking that %s includes only the raw stack key's files" % src.short_path,
            _UNITS[src.extension] + includes + ["-MM", "-MF", out.path, src.path],
            [src] + headers,
            [out],
        )
        checked.append(out)
    return checked

def _raw_stack_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.name + ".txt")
    files = depset(
        ctx.files.srcs + ctx.files.readers + [ctx.file.metal_abi, ctx.file.nvk],
        transitive = [ctx.attr.bridge[_CrateSourcesInfo].files],
    )
    args = ctx.actions.args()
    args.use_param_file("@%s", use_always = True)
    args.set_param_file_format("multiline")
    args.add("--ipsw", ctx.attr.ipsw[IpswInfo].build)
    args.add("--out", out)
    args.add_all(files, map_each = _hashed_as, before_each = "--file")
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = files,
        outputs = [out],
        mnemonic = "RawStack",
        progress_message = "Hashing the raw path's stack key",
    )
    listing = ctx.actions.declare_file(ctx.attr.name + ".inputs")
    names = ctx.actions.args()
    names.set_param_file_format("multiline")
    names.add_all(files, map_each = _short_path)
    ctx.actions.write(listing, names)
    return [
        DefaultInfo(files = depset([out])),
        OutputGroupInfo(inputs = depset([listing]), _validation = depset(_includes_checked(ctx))),
    ]

raw_stack = rule(
    implementation = _raw_stack_impl,
    doc = """The raw path's stack key (tools/gpu/raw_stack.py): what its records are made and read by.

    srcs and readers, by path and bytes, with NVK, MetalABI.h, every crate the Metal bridge compiles (its sources,
    compile data and build scripts) and the IPSW build. Each C, C++ and Objective-C++ unit of srcs is checked to
    include only srcs and MetalABI.h; the hashed files' paths are the output group `inputs`.""",
    attrs = _XCODE | {
        "srcs": attr.label_list(
            mandatory = True,
            allow_files = True,
            doc = "what records, pack entries and native records are made by",
        ),
        "readers": attr.label_list(allow_files = True, doc = "files that only read the records the device made"),
        "bridge": attr.label(mandatory = True, aspects = [_crate_sources], doc = "the Metal bridge (its crate graph)"),
        "include_markers": attr.label_list(allow_files = True, doc = "one file per -I directory of srcs' units"),
        "metal_abi": attr.label(mandatory = True, allow_single_file = True, doc = "the IPSW kind's MetalABI.h"),
        "nvk": attr.label(mandatory = True, allow_single_file = True),
        "ipsw": attr.label(default = "//ipsw:current", providers = [IpswInfo]),
        "_tool": attr.label(default = "//tools/gpu:raw_stack", executable = True, cfg = "exec"),
        "_xcode_run": attr.label(default = "//tools/gpu:xcode_run", executable = True, cfg = "exec"),
    },
)

# --- The signed bundle and its trust cache -----------------------------------------------------------------------

def _metal_bundle_impl(ctx):
    bundle = ctx.actions.declare_directory("TegraGM20BMetal.bundle")
    trustcache = ctx.actions.declare_file("TegraGM20BMetal.trustcache")
    trust_json = ctx.actions.declare_file("trust.json")
    args = ctx.actions.args()
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--plugin", ctx.file.plugin)
    args.add("--nvk", ctx.file.nvk)
    args.add("--stack", ctx.file.stack)
    args.add("--bundle", bundle.path)
    args.add("--trustcache", trustcache)
    args.add("--trust-json", trust_json)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [ctx.file.plugin, ctx.file.nvk, ctx.file.stack, ctx.file._xcode_env, ctx.file._xcode_identity],
        outputs = [bundle, trustcache, trust_json],
        mnemonic = "MetalBundle",
        progress_message = "Signing TegraGM20BMetal.bundle and writing its trust cache",
    )
    return [
        DefaultInfo(files = depset([bundle, trustcache, trust_json])),
        OutputGroupInfo(bundle = depset([bundle]), trustcache = depset([trustcache]), trust_json = depset([trust_json])),
    ]

metal_bundle = rule(
    implementation = _metal_bundle_impl,
    attrs = _XCODE | {
        "plugin": attr.label(mandatory = True, allow_single_file = True),
        "nvk": attr.label(mandatory = True, allow_single_file = True),
        "stack": attr.label(mandatory = True, allow_single_file = True, doc = "the raw path's stack key, into Info.plist"),
        "_tool": attr.label(default = "//tools/gpu:bundle", executable = True, cfg = "exec"),
    },
)

# --- The directory deploy stages ---------------------------------------------------------------------------------

def _gpu_tree_impl(ctx):
    out = ctx.actions.declare_directory(ctx.attr.name)
    bundle = [f for f in ctx.files.bundle if f.is_directory][0]
    args = ctx.actions.args()
    args.add("--bundle", bundle.path)
    args.add("--trustcache", ctx.file.trustcache)
    args.add("--trust-json", ctx.file.trust_json)
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [bundle, ctx.file.trustcache, ctx.file.trust_json],
        outputs = [out],
        mnemonic = "GpuTree",
        progress_message = "Laying out the GPU bundle directory",
    )
    return [DefaultInfo(files = depset([out]))]

gpu_tree = rule(
    implementation = _gpu_tree_impl,
    attrs = {
        "bundle": attr.label(mandatory = True),
        "trustcache": attr.label(mandatory = True, allow_single_file = True),
        "trust_json": attr.label(mandatory = True, allow_single_file = True),
        "_tool": attr.label(default = "//tools/gpu:assemble", executable = True, cfg = "exec"),
    },
)

# --- What deploy stages: a manifest over the outputs, nothing copied ----------------------------------------------

def _rlocation(ctx, f):
    return f.short_path[3:] if f.short_path.startswith("../") else ctx.workspace_name + "/" + f.short_path

def _gpu_stage_impl(ctx):
    seed = ctx.attr.seed[SeedInfo]
    base = [f for f in ctx.files.base if f.is_directory][0]

    # The seed's lines on their own, so that the plugin, which most edits change, is all the manifest hashes again.
    part = ctx.actions.declare_file(ctx.attr.name + ".stage.seed")
    args = ctx.actions.args()
    args.add("--part")
    for flag, tree in (("--seed", seed.meta), ("--raw", ctx.file.raw), ("--native", seed.native)):
        args.add(flag, tree.path)
        args.add(flag + "-rlocation", _rlocation(ctx, tree))
    for pack in seed.packs:
        args.add("--pack", pack)
        args.add("--pack-rlocation", _rlocation(ctx, pack))
    args.add("--out", part)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [seed.meta, seed.native, ctx.file.raw] + seed.packs,
        outputs = [part],
        mnemonic = "GpuStageSeed",
        progress_message = "Listing the shader seed and the raw pack for the Data volume",
    )

    manifest = ctx.actions.declare_file(ctx.attr.name + ".stage")
    args = ctx.actions.args()
    args.add("--self", _rlocation(ctx, manifest))
    args.add("--base", base.path)
    args.add("--base-rlocation", _rlocation(ctx, base))
    args.add("--from-part", part)
    args.add("--out", manifest)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [base, part],
        outputs = [manifest],
        mnemonic = "GpuStageManifest",
        progress_message = "Listing the GPU bundle, the shader seed and the raw pack for the Data volume",
    )
    files = [manifest, base, seed.meta, seed.native, ctx.file.raw] + seed.packs
    return [DefaultInfo(files = depset([manifest]), runfiles = ctx.runfiles(files = files))]

gpu_stage = rule(
    implementation = _gpu_stage_impl,
    doc = "The file deploy's stager syncs the Data volume to: every file, its sha256 and where it is in the runfiles.",
    attrs = {
        "base": attr.label(mandatory = True, doc = "//gpu:base: the signed bundle"),
        "seed": attr.label(mandatory = True, providers = [SeedInfo]),
        "raw": attr.label(mandatory = True, allow_single_file = True, doc = "the raw pack's directory"),
        "_tool": attr.label(default = "//tools/gpu:stage_manifest", executable = True, cfg = "exec"),
    },
)
