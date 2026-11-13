"""XNU compiled natively: every object is its own rules_cc compile action on the kernel toolchain, so editing one XNU
source recompiles that object and relinks, nothing else.

A component (osfmk, bsd, ...) is one xnu_objects target over its table (<component>/objects.bzl): its objects in
make's link order, each compiled with XNU's recipe for the component and language (recipes_<config>.bzl, make's
CFLAGS + CWARNFLAGS, then the object's own copts, then the recipe's INCFLAGS, then the object's own includes: the
-I order decides which copy of a header wins), then the setsegname passes make runs on it.

Strings. Every path that reaches an object (DWARF names and directories, __FILE__) is written as make wrote it for
main's kernel, so the kernel stays bit-identical to the one the make flow linked: -fdebug-prefix-map (clang takes the
last match) and -fmacro-prefix-map (the most specific) map each input directory onto its old spelling (LEGACY), and
-fdebug-compilation-dir names make's object directory. Sources make compiled by a relative path (MIG, iig and
makesyscalls output, from the component's object directory) map their directory to "", and `-I .` (the object
directory) is spelled <gen>/. and maps to "./", as make's relative spellings were.

Placeholders in recipes and tables: {xnu} the XNU source, {export} EXPORT_HDRS, {gen} the component's generated
directory (make's `.`), {sdkroot} the kernel SDK root, {san} the sanitizer ignore lists, {tree} the kernel tree,
{owner_header} bsd's owner-data-volume.h, {kernel_virt_base} the IPSW kind's kernel window base.
"""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@xcode//:defs.bzl", "DEVELOPER_DIR", "XCODE_TOOLCHAIN")
load("//bazel/platforms:transitions.bzl", "kernel_transition", "outer_transition")
load("//ipsw:ipsw.bzl", "kernel_virt_base")
load("//kernel/rules:layout.bzl", "MOVED_ROOTS", "OLD_TREE", "PATH_ROOT", "tree_path")
load("//kernel/switch:objects.bzl", "SWITCH_ROOT", "switch_copts", "switch_objects", "with_kprof")
load(":recipes_development.bzl", "DEVELOPMENT_RECIPES")
load(":recipes_release.bzl", "RELEASE_RECIPES")
load(":selected.bzl", "selected_only")
load(":switch_objects.bzl", "SWITCH_COPTS")

XnuObjectsInfo = provider(
    doc = "A component's objects in link order, under the names make gave them.",
    fields = {
        "component": "osfmk, bsd, ... (config for the kernel's own objects)",
        "config": "the XNU kernel config (DEVELOPMENT, RELEASE)",
        "names": "make's object names (ast.o, IOService.cpo, ...), parallel to objects",
        "objects": "list of File, link order",
        "symbols": "per object, the names it defines (\"g _x\" global, \"a _x\" any), parallel to objects",
    },
)

RECIPES = {
    "DEVELOPMENT": DEVELOPMENT_RECIPES,
    "RELEASE": RELEASE_RECIPES,
}

# make's spellings (main's kernel, built by make's xnu_build.py with -ffile-prefix-map=<execroot>=/switch-ios).
_ROOT = "/switch-ios"
_MAKE_OBJ = "out/native-a57/root-v1/tree/build/xnu/ios-obj"
_MAKE_BIN = "bazel-out/darwin_arm64-fastbuild/bin"
LEGACY = struct(
    # (debug map target, macro map target) per input directory.
    xnu = (_ROOT + "/kernel/xnu-12377.121.6", _ROOT + "/kernel/xnu-12377.121.6"),
    export = ("{}/{}/{}/EXPORT_HDRS".format(_ROOT, _MAKE_BIN, _MAKE_OBJ), "{}/{}/EXPORT_HDRS".format(_ROOT, _MAKE_OBJ)),
    sdkroot = (
        "{}/{}/out/native-a57/root-v1/tree/.cache/fakeroot".format(_ROOT, _MAKE_BIN),
        _ROOT + "/out/native-a57/root-v1/tree/.cache/fakeroot",
    ),
    owner = ("{}/{}/out/native-a57/root-v1/tree/build/xnu".format(_ROOT, _MAKE_BIN), _ROOT + "/out/native-a57/root-v1/tree/build/xnu"),
    obj = ("{}/{}/{}".format(_ROOT, _MAKE_BIN, _MAKE_OBJ), "{}/{}".format(_ROOT, _MAKE_OBJ)),
)

# The flags only the kernel toolchain's driver recipes add: never on XNU's own objects. (xnu_version_macro is on:
# XNU_<version>, which kernel/switch's code and hook headers test.)
_DRIVER_FEATURES = ["kernel_iokit_cxx", "kernel_osfmk_c", "kernel_warnings", "kernel_plain_asm"]

_LANG = {"c": "c", "cpp": "cxx", "s": "asm"}

def _one(target):
    if type(target) == "list":
        return target[0] if target else None
    return target

def _files(attr):
    target = _one(attr)
    return target.files.to_list() if target else []

def _dir_of(files, suffix, what):
    """The directory a file group's files hang from: the prefix of the first path containing suffix."""
    for f in files:
        at = f.path.find(suffix)
        if at >= 0:
            return f.path[:at]
    fail("no {} ({}) in {}".format(what, suffix, [f.path for f in files[:3]]))

def _maps(places, gen, relative):
    """-fdebug-prefix-map/-fmacro-prefix-map flags: every input directory to its make spelling."""

    # The kernel tree's directories XNU includes from, wherever kernel/rules/layout.bzl puts them now.
    tree = [(tree_path(root), ("{}/{}/{}".format(PATH_ROOT, OLD_TREE, root),) * 2) for root in MOVED_ROOTS]
    tree.append((tree_path(SWITCH_ROOT), ("{}/{}".format(PATH_ROOT, SWITCH_ROOT),) * 2))
    pairs = [(places["{xnu}"], LEGACY.xnu)] + tree + [
        (places["{export}"], LEGACY.export),
        (places["{sdkroot}"], LEGACY.sdkroot),
        (places["{owner_dir}"], LEGACY.owner),
    ]
    flags = []
    for kind, index in (("debug", 0), ("macro", 1)):
        for path, legacy in pairs:
            flags.append("-f{}-prefix-map={}/={}/".format(kind, path, legacy[index]))

        # The component's own directory: "" (make's relative spellings) or make's absolute one (config's version.c).
        flags.append("-f{}-prefix-map={}/={}".format(kind, gen.path, "" if relative else gen.legacy[index] + "/"))
        flags.append("-f{}-prefix-map={}/./=./".format(kind, gen.path))
    return flags

def _expand(values, places):
    out = []
    for value in values:
        for key, path in places.items():
            value = value.replace(key, path)
        out.append(value)
    return out

# make's assym.s rule, verbatim.
_ASSYM_SED = [
    "-e",
    "/^[[:space:]]*DEFINITION__define__/!d;{N;s/\\n//;}",
    "-e",
    "s/^[[:space:]]*DEFINITION__define__\\([^:]*\\):.*ascii.*\\\"[\\$]*\\([-0-9\\#]*\\)\\\".*$/#define \\1 \\2/",
    "-e",
    "p",
    "-e",
    "s/#//2",
    "-e",
    "s/^[[:space:]]*#define \\([A-Za-z0-9_]*\\)[[:space:]]*[\\$#]*\\([-0-9]*\\).*$/#define \\1_NUM \\2/",
]

def _genassym(ctx, feature_configuration, cc_toolchain, recipe, places, maps, compilation_context, ignorelists):
    flags = [f for f in _expand(recipe.flags, places) if f != "-c"]

    # make's ${CFLAGS_NOLTO_FLAG}: -fno-lto whenever the config builds LTO (RELEASE), or -S writes LLVM IR, not the
    # assembly the sed below reads.
    if [f for f in flags if f.startswith("-flto")]:
        flags.append("-fno-lto")
    flags += ["-fno-integrated-as", "-S"] + _expand(recipe.includes, places) + maps
    _, outputs = cc_common.compile(
        name = ctx.label.name + "_genassym",
        actions = ctx.actions,
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        srcs = [ctx.file.genassym],
        user_compile_flags = flags,
        compilation_contexts = [compilation_context],
        additional_inputs = ignorelists,
    )
    assembly = outputs.objects[0]
    assym = ctx.actions.declare_file(ctx.label.name + "_assym/assym.s")
    ctx.actions.run_shell(
        inputs = [assembly],
        outputs = [assym],
        arguments = _ASSYM_SED + [assembly.path, assym.path],
        command = 'out="${@: -1}"; set -- "${@:1:$#-1}"; /usr/bin/sed "$@" > "$out"',
        mnemonic = "XnuAssym",
        progress_message = "Making assym.s from genassym.c's assembly",
    )
    return assym

def _symbols(ctx, obj, base):
    """The names one object defines, as kernel_names.py lists them (nm -g -U -j: "g", nm -U -j: "a").

    One small cached action per object, so after an edit only that object's list is redone before the merge
    (//kernel:xnu_symbols)."""
    out = ctx.actions.declare_file("{}_symbols/{}.names".format(ctx.label.name, base))
    ctx.actions.run_shell(
        inputs = [obj],
        outputs = [out],
        arguments = [XCODE_TOOLCHAIN + "/nm", obj.path, out.path],
        command = """set -o pipefail
{ "$1" -g -U -j "$2" | /usr/bin/sed -n 's/^\\(_.*\\)$/g \\1/p'; "$1" -U -j "$2" | /usr/bin/sed -n 's/^\\(_.*\\)$/a \\1/p'; } > "$3"
""",
        env = {"DEVELOPER_DIR": DEVELOPER_DIR, "LC_ALL": "C", "PATH": "/usr/bin:/bin"},
        mnemonic = "XnuSymbols",
        progress_message = "Listing the names {} defines".format(base),
        execution_requirements = {"requires-darwin": ""},
    )
    return out

def _xnu_objects_impl(ctx):
    cc_toolchain = find_cc_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features + _DRIVER_FEATURES,
    )
    config = ctx.attr.config
    recipes = RECIPES[config]
    comp = ctx.attr.component
    xnu = ctx.label.package.rpartition("/")[0]
    gen_srcs = _files(ctx.attr.gen_srcs)
    gen_hdrs = _files(ctx.attr.gen_hdrs)
    export_hdrs = _files(ctx.attr.export_hdrs)
    sdkroot = _files(ctx.attr.sdkroot)
    owner = _one(ctx.attr.owner_header).files.to_list()[0]
    ignorelists = _files(ctx.attr.ignorelists)
    objdir = "{}_ARM64_BCM2837".format(config)
    if comp == "config":
        gen_path = gen_srcs[0].dirname
        legacy_gen = tuple([v + "/" + objdir for v in LEGACY.obj])
    else:
        gen_path = _dir_of(gen_hdrs, "/meta_features.h", "meta_features.h")
        legacy_gen = tuple(["{}/{}/{}/{}".format(v, objdir, comp, config) for v in LEGACY.obj])
    gen = struct(path = gen_path, legacy = legacy_gen)
    places = {
        "{export}": _dir_of(export_hdrs, "/EXPORT_HDRS/", "EXPORT_HDRS") + "/EXPORT_HDRS",
        "{kernel_virt_base}": kernel_virt_base(ctx.attr._kind[BuildSettingInfo].value),
        "{gen}": gen_path,
        "{owner_dir}": owner.dirname,
        "{owner_header}": owner.path,
        "{san}": ignorelists[0].dirname,
        "{sdkroot}": _dir_of(sdkroot, "/usr/local/include/", "the SDK root's usr/local/include"),
        "{tree}/": tree_path(""),
        "{xnu}": xnu,
    }
    maps = _maps(places, gen, ctx.attr.gen_relative) + [
        "-fdebug-compilation-dir=" + legacy_gen[0],
    ]

    # Everything a compile may include (.d files prune each action to what it really read).
    headers = depset(
        gen_hdrs + export_hdrs + sdkroot + [owner] + ctx.files.tree_headers,
        transitive = [t.files for t in ctx.attr.xnu_headers],
    )
    compilation_context = cc_common.create_compilation_context(headers = headers)

    # assym.s (osfmk/conf/Makefile.template): genassym.c compiled to assembly with the component's C recipe and -S
    # (make names that output genassym.o), then make's sed turns its DEFINITION__define__ strings into #defines. The
    # assembly sources find it next to make's `-I .`, in a directory of its own (no other header lives there).
    asm_includes = []
    if ctx.file.genassym:
        assym = _genassym(
            ctx,
            feature_configuration,
            cc_toolchain,
            recipes["{}_c".format(comp)],
            places,
            maps,
            compilation_context,
            ignorelists,
        )
        compilation_context = cc_common.create_compilation_context(headers = depset([assym], transitive = [headers]))
        asm_includes = ["-I", assym.dirname]

    by_path = {}
    for f in ctx.files.srcs:
        if not f.short_path.startswith(xnu + "/"):
            fail("{}: {} is not in {}".format(ctx.label, f.short_path, xnu))
        by_path[f.short_path[len(xnu) + 1:]] = f
    for f in gen_srcs:
        if f.path.startswith(gen_path + "/"):
            by_path["gen:" + f.path[len(gen_path) + 1:]] = f
    for f in ctx.files.switch_srcs:
        if f.short_path.startswith(SWITCH_ROOT + "/"):
            by_path["switch:" + f.short_path[len(SWITCH_ROOT) + 1:]] = f
        else:
            by_path["switch://" + f.short_path] = f

    # One compile per set of identical flags (most of a component shares its recipe's).
    groups, order = {}, []
    sources = []
    for entry in ctx.attr.objects:
        src = by_path.get(entry)
        if not src:
            fail("{}: no source for {} (srcs, gen_srcs)".format(ctx.label, entry))
        lang = _LANG.get(src.extension)
        if not lang:
            fail("{}: {} is not C, C++ or assembly".format(ctx.label, entry))
        recipe = recipes.get("{}_{}".format("kernel" if comp == "config" else comp, lang))
        if not recipe:
            fail("{}: no recipe {}_{} for config {}".format(ctx.label, comp, lang, config))
        includes = _expand(recipe.includes, places)
        if lang == "asm" and asm_includes:
            at = includes.index(gen_path + "/.") + 1
            includes = includes[:at] + asm_includes + includes[at:]
        flags = (
            _expand(recipe.flags, places) +
            _expand(ctx.attr.copts.get(entry, []), places) +
            includes +
            _expand(ctx.attr.includes.get(entry, []), places) +
            ["-I", tree_path(SWITCH_ROOT + "/include")] +
            maps
        )
        key = "\n".join(flags)
        if entry.startswith("switch:"):
            # A compile of its own per kernel/switch directory: no object of ours shares a name with an Apple one.
            key = entry.rpartition("/")[0] + "\n" + key
        if key not in groups:
            groups[key] = struct(name = "{}_{}".format(ctx.label.name, len(order)), flags = flags, srcs = [])
            order.append(key)
        groups[key].srcs.append(src)
        sources.append((entry, src))

    compiled = {}
    for key in order:
        group = groups[key]
        _, outputs = cc_common.compile(
            name = group.name,
            actions = ctx.actions,
            feature_configuration = feature_configuration,
            cc_toolchain = cc_toolchain,
            srcs = group.srcs,
            user_compile_flags = group.flags,
            compilation_contexts = [compilation_context],
            additional_inputs = ignorelists,
        )
        objects = outputs.objects
        if len(objects) != len(group.srcs):
            fail("{}: {} sources, {} objects".format(ctx.label, len(group.srcs), len(objects)))
        for src, obj in zip(group.srcs, objects):
            compiled[src] = obj

    objects, names, symbols = [], [], []
    setsegname = ctx.executable._setsegname
    for entry, src in sources:
        obj = compiled[src]

        # kernel/switch's objects keep their directory: switch/<subsystem>/<name>.o, never an Apple object's name.
        stem = "switch/" + entry[len("switch:"):].lstrip("/") if entry.startswith("switch:") else src.basename
        base = stem.rpartition(".")[0] + (".cpo" if src.extension == "cpp" else ".o")
        for i, spec in enumerate(ctx.attr.segments.get(entry, [])):
            renamed = ctx.actions.declare_file("{}_seg/{}.{}".format(ctx.label.name, base, i))
            args = ctx.actions.args()
            args.add_all(spec.split(" "))
            args.add("-o", renamed)
            args.add(obj)
            ctx.actions.run(
                executable = setsegname,
                arguments = [args],
                inputs = [obj],
                outputs = [renamed],
                mnemonic = "XnuSetSegName",
                progress_message = "setsegname {} {}".format(spec, base),
            )
            obj = renamed
        objects.append(obj)
        names.append(base)
        symbols.append(_symbols(ctx, obj, base))
    return [
        DefaultInfo(files = depset(objects)),
        XnuObjectsInfo(component = comp, objects = objects, names = names, symbols = symbols, config = config),
    ]

xnu_objects_rule = rule(
    implementation = _xnu_objects_impl,
    doc = "One XNU component's objects (see the file's docstring), compiled for the kernel platform.",
    cfg = kernel_transition,
    attrs = {
        "component": attr.string(mandatory = True),
        "config": attr.string(default = "DEVELOPMENT", values = ["DEVELOPMENT", "RELEASE"]),
        "copts": attr.string_list_dict(doc = "per object: flags after the recipe's (make's _CFLAGS_ADD, _CWARNFLAGS_ADD)"),
        "export_hdrs": attr.label(mandatory = True, allow_files = True, cfg = outer_transition),
        "gen_hdrs": attr.label(allow_files = True, cfg = outer_transition, doc = "the component's generated headers"),
        "gen_relative": attr.bool(default = True, doc = "make compiled gen: sources by a relative path"),
        "gen_srcs": attr.label(allow_files = True, cfg = outer_transition, doc = "generated sources (gen:...)"),
        "genassym": attr.label(allow_single_file = [".c"], doc = "osfmk only: genassym.c (assym.s)"),
        "ignorelists": attr.label(mandatory = True, allow_files = True, cfg = outer_transition),
        "includes": attr.string_list_dict(doc = "per object: flags after the recipe's INCFLAGS (make's _INCFLAGS)"),
        "objects": attr.string_list(mandatory = True, doc = "XNU-relative sources (or gen:<path>, switch:<path>), link order"),
        "owner_header": attr.label(mandatory = True, allow_single_file = True, cfg = outer_transition),
        "sdkroot": attr.label(mandatory = True, allow_files = True, cfg = outer_transition),
        "segments": attr.string_list_dict(doc = "per object: setsegname passes (arguments), in order"),
        "srcs": attr.label_list(allow_files = [".c", ".cpp", ".s"], doc = "the vendored sources objects names"),
        "switch_srcs": attr.label_list(allow_files = [".c", ".cpp", ".s"], doc = "kernel/switch's sources (switch:<path>)"),
        "tree_headers": attr.label_list(allow_files = True, doc = "the kernel tree's headers XNU includes"),
        "xnu_headers": attr.label_list(allow_files = True, doc = "XNU's source headers (every component's :headers)"),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
        "_kind": attr.label(default = "//ipsw:kind"),
        "_setsegname": attr.label(default = "//kernel/xnu-12377.121.6:setsegname", executable = True, cfg = "exec"),
    },
    toolchains = use_cc_toolchain(),
    fragments = ["cpp"],
)

# The inputs every component reads: the top-level package's EXPORT_HDRS, generated files and SDK root (r18-xnu-hdrs').
XNU_INPUTS = "//kernel/xnu-12377.121.6:"

# XNU's kernel config per build profile (//bazel:profile): debug DEVELOPMENT, release RELEASE.
def by_profile(development, release):
    return select({
        "//bazel:release": release,
        "//conditions:default": development,
    })

def xnu_objects(name, component, table, release, srcs = None, **kwargs):
    """A component's objects for the build profile's XNU configuration.

    Args:
        name: the target (every component's is "objects").
        component: osfmk, bsd, ... or config (the kernel's own objects).
        table: the component's DEVELOPMENT table (objects.bzl struct: objects, copts, includes, segments); each
            object's SWITCH_COPTS go before its copts.
        release: its RELEASE table, with the same SWITCH_COPTS ordering.
        srcs: the vendored sources the tables name (default: the package's .c, .cpp and .s files).
        **kwargs: input overrides (config's objects: gen_srcs = version.c, gen_hdrs = None, gen_relative = False).
    """
    inputs = {
        "gen_hdrs": XNU_INPUTS + "gen_{}_hdrs".format(component),
        "gen_srcs": XNU_INPUTS + "gen_{}_srcs".format(component),
    }
    inputs.update(kwargs)

    # kernel/switch's objects join each component's own objects, not pthread_bootstrap's one.
    ours = component if name == "objects" else None
    own_copts = {k: SWITCH_COPTS.get(k, []) + table.copts.get(k, []) for k in table.objects}
    release_copts = {k: SWITCH_COPTS.get(k, []) + release.copts.get(k, []) for k in release.objects}
    xnu_objects_rule(
        name = name,
        component = component,
        config = by_profile("DEVELOPMENT", "RELEASE"),
        objects = by_profile(switch_objects(ours, table.objects), switch_objects(ours, release.objects)),
        copts = select({
            "//kernel:kprof_debug": with_kprof(component, switch_copts(ours, own_copts)),
            "//bazel:release": switch_copts(ours, release_copts),
            "//conditions:default": switch_copts(ours, own_copts),
        }),
        includes = by_profile(table.includes, release.includes),
        segments = by_profile(table.segments, release.segments),
        target_compatible_with = selected_only(),
        srcs = srcs if srcs != None else native.glob(["**/*.c", "**/*.cpp", "**/*.s"], allow_empty = True),
        switch_srcs = ["//kernel/switch:srcs"],
        export_hdrs = XNU_INPUTS + "export_hdrs",
        sdkroot = XNU_INPUTS + "sdkroot_files",
        ignorelists = "//kernel/xnu-12377.121.6/san:ignorelists",
        owner_header = "//kernel/xnu-12377.121.6/bsd:owner_header",
        xnu_headers = ["//kernel/xnu-12377.121.6/{}:headers".format(c) for c in XNU_COMPONENTS] + [
            XNU_INPUTS + "external_headers",
        ],
        # The kernel tree's headers XNU's own sources include: the diagnostics (-I diagnostics/switch) and, through
        # SwitchRawMount.h, the block device's and the verified root's.
        tree_headers = [
            "//diagnostics/switch:headers",
            "//drivers/NativeSwitchBlock:headers",
            "//kits/VerifiedRoot:headers",
            "//kernel/switch:headers",
        ],
        # XNU is Apple's code: no clang-tidy or other linting of its compiles.
        tags = [
            "kernel",
            "no-lint",
        ],
        **inputs
    )

XNU_COMPONENTS = ["osfmk", "bsd", "iokit", "libkern", "libsa", "pexpert", "san", "security"]
