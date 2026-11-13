"""XNU's headers and generated sources as Bazel actions, from the tables xnu_record.py records from XNU's make.

A table lists every step make takes besides compiling (installfile copies, unifdef installs, mig, iig, doconf,
makesyscalls.sh, newvers.pl, ...) with the files it reads and writes. xnu_pass replays one table: each output is its own
file artifact (never a tree), copies and installs are batched per directory, every other step is its own action, and
mig/iig list the headers they did not read (unused_inputs_list), so editing one header reruns only the steps that read
it, and then only the compiles that include what changed.

  xnu_pass       one recorded make pass (installhdrs: the kernel SDK root; build: XNU's object root)
  xnu_host_tool  a host tool from XNU's SETUP/ (config, decomment, setsegname, kextsymboltool), with Xcode's clang
  xnu_sdk        what the kernel's compiles include (KernelSdkInfo + CcInfo for //kernel:sdk)
  xnu_sources    every file of the XNU tree, across the packages r18-xnu-objs' BUILD files make of it

Version-independent: kernel/xnu-<version>/BUILD.bazel instantiates these with its own tables.
"""

load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@xcode//:defs.bzl", "DEVELOPER_DIR", "MACOSX_SDK")
load("//bazel/platforms:transitions.bzl", "outer_transition")
load("//kernel/rules:common.bzl", "PY_TOOLCHAIN", "XCODE_EXECUTION")
load("//kernel/rules:layout.bzl", "OLD_TREE", "TREE_ROOT")
load("//kernel/rules:sdk.bzl", "KernelSdkInfo")

XnuTreeInfo = provider(
    doc = "Files below one directory, by their path below it.",
    fields = {
        "base": "dict: the subset an earlier step wrote that later steps of the same pass may read (installhdrs: " +
                "the SDK root's other parts)",
        "files": "dict: path below root -> File",
        "root": "the directory's exec path (bazel-out/<cfg>/bin/<package>/<dir>)",
    },
)

_TOOLCHAIN = DEVELOPER_DIR + "/Toolchains/XcodeDefault.xctoolchain/usr/bin"

# What the include directories of mig and iig contribute from the source: headers and interface definitions.
_SOURCE_HEADERS = ("h", "hpp", "defs", "iig", "inc", "def", "modulemap")

# Steps batched into one action per output directory (one input, one output each: cheap, and a batch's unchanged
# outputs keep their digests, so dependents do not rerun).
_BATCHED = ("copy", "unifdef", "unifdef_to")

def _split(key):
    root, _, rest = key.partition("/")
    return root, rest

def _dirname(path):
    return path.rpartition("/")[0]

def _xnu_pass_impl(ctx):
    recorded = json.decode(ctx.attr.table)
    table = []
    for step in recorded["steps"]:
        if "flags" in step:
            step = dict(step)
            step["flags"] = recorded["flags"][step["flags"]]
        table.append(step)
    xnu_root = ctx.attr.xnu_root
    sources = {}
    for f in ctx.files.srcs:
        if f.path.startswith(xnu_root + "/"):
            sources[f.path[len(xnu_root) + 1:]] = f
    sdk = ctx.attr.sdkroot[XnuTreeInfo] if ctx.attr.sdkroot else None
    package_dir = ctx.label.package
    dirs = {"d": ctx.attr.dst_dir, "o": ctx.attr.obj_dir}
    bin_dir = ctx.bin_dir.path
    roots = {
        "d": "{}/{}/{}".format(bin_dir, package_dir, ctx.attr.dst_dir) if ctx.attr.dst_dir else None,
        "k": sdk.root if sdk else None,
        "o": "{}/{}/{}".format(bin_dir, package_dir, ctx.attr.obj_dir),
        "s": xnu_root,
        "x": DEVELOPER_DIR,
    }

    # In installhdrs, KERNEL_SDKROOT is the DSTROOT being filled ({d}); later steps may read what the base step wrote.
    if not sdk and ctx.attr.dst_dir:
        roots["k"] = roots["d"]

    # Declare every output.
    outputs = {}
    for step in table:
        for key in step.get("outs", []) + ([step["out"]] if "out" in step else []):
            root, rest = _split(key)
            if root not in dirs or not dirs[root]:
                fail("{}: step writes outside the pass's roots: {}".format(ctx.label, key))
            path = dirs[root] + "/" + rest
            if key in outputs:
                fail("{}: {} written by two steps".format(ctx.label, key))
            outputs[key] = ctx.actions.declare_symlink(path) if step["kind"] == "symlink" else ctx.actions.declare_file(path)
    base = {}
    for step in table:
        if step["kind"] == "base":
            for key in step["outs"]:
                base[key] = outputs[key]

    def lookup(key):
        root, rest = _split(key)
        if root == "s":
            f = sources.get(rest)
        elif root in ("o", "d"):
            f = outputs.get(key)
        elif root == "k":
            f = sdk.files.get(rest) if sdk else (outputs.get("d/" + rest))
        else:
            f = None
        if f == None:
            fail("{}: no file for {} (is the table current? bazel run :update_tables)".format(ctx.label, key))
        return f

    # The files below an include directory, for mig/iig: sources broadly (pruned at run time), the SDK root's files
    # broadly, this pass's own outputs only through the step's recorded dependencies (broad, they would form cycles).
    below_cache = {}

    def below(incdir):
        if incdir in below_cache:
            return below_cache[incdir]
        root, rest = _split(incdir)
        prefix = rest + "/" if rest and rest != "." else ""
        found = []
        if root == "s":
            found = [f for p, f in sources.items() if p.startswith(prefix) and p.rpartition(".")[2] in _SOURCE_HEADERS]
        elif root == "k" and sdk:
            found = [f for p, f in sdk.files.items() if p.startswith(prefix)]
        elif root in ("k", "d") and not sdk:
            found = [f for k, f in base.items() if k[2:].startswith(prefix)]
        below_cache[incdir] = found
        return found

    tools = {"config": ctx.executable.config.path, "decomment": ctx.executable.decomment.path}
    tool_inputs = [ctx.executable.decomment, ctx.executable.config, ctx.file._xcode_identity]
    py = ctx.toolchains[PY_TOOLCHAIN].py3_runtime
    base_sources = {}
    for name in ("availability", "libplatform", "libdispatch"):
        files = getattr(ctx.files, name)
        if files:
            base_sources[name] = files[0].owner.workspace_root

    batches = {}
    singles = []
    for step in table:
        kind = step["kind"]
        if kind == "symlink":
            ctx.actions.symlink(output = outputs[step["out"]], target_path = step["target"])
        elif kind in _BATCHED:
            batches.setdefault((kind, _dirname(step["out"])), []).append(step)
        else:
            singles.append(step)

    actions = [(kind, steps) for (kind, _), steps in sorted(batches.items())] + [(s["kind"], [s]) for s in singles]
    for index, (kind, steps) in enumerate(actions):
        inputs, outs = [], []
        pruned = []
        for step in steps:
            outs += [outputs[k] for k in step.get("outs", []) + ([step["out"]] if "out" in step else [])]
            if "src" in step:
                inputs.append(lookup(step["src"]))
            inputs += [lookup(k) for k in step.get("srcs", [])]
            if step.get("pre_copy"):
                inputs.append(lookup(step["pre_copy"]))
            if kind == "script":
                inputs.append(lookup(step["tool"]))
                inputs += [lookup(k) for k in step["ins"]]
            if kind in ("mig", "iig"):
                inputs += [lookup(k) for k in step["deps"]]
                for incdir in step["incdirs"]:
                    pruned += below(incdir)
            if kind == "base":
                for name in ("availability", "libplatform", "libdispatch"):
                    inputs += getattr(ctx.files, name)
        own = {o.path: True for o in outs}
        pruned = [f for f in depset(pruned).to_list() if f.path not in own]
        spec = ctx.actions.declare_file("steps/{}/{}-{}.json".format(ctx.label.name, index, kind))
        data = {"base_sources": base_sources, "roots": roots, "steps": steps, "tools": tools}
        unused = None
        if kind in ("mig", "iig"):
            unused = ctx.actions.declare_file("steps/{}/{}-{}.unused".format(ctx.label.name, index, kind))
            data["unused"] = unused.path
        ctx.actions.write(spec, json.encode(data))
        args = ctx.actions.args()
        args.add_all(["-I", ctx.file._ops, spec])
        arguments = [args]
        if unused:
            # The include directories' files, the candidates for unused_inputs_list, in a parameter file.
            listed = ctx.actions.args()
            listed.add_all(pruned)
            listed.use_param_file("@%s", use_always = True)
            listed.set_param_file_format("multiline")
            arguments.append(listed)
        ctx.actions.run(
            executable = py.interpreter,
            arguments = arguments,
            inputs = depset([spec, ctx.file._ops] + inputs + pruned + tool_inputs, transitive = [py.files]),
            outputs = outs + ([unused] if unused else []),
            unused_inputs_list = unused,
            mnemonic = "Xnu" + kind.capitalize().replace("_", ""),
            progress_message = "XNU {} {}".format(kind, outs[0].short_path if len(outs) == 1 else _dirname(outs[0].short_path)),
            execution_requirements = XCODE_EXECUTION,
        )

    files = {}
    for key, f in outputs.items():
        files[key] = f
    by_root = {}
    for key, f in files.items():
        root, rest = _split(key)
        by_root.setdefault(root, {})[rest] = f
    groups = {}
    for key, f in files.items():
        root, rest = _split(key)
        for group in _groups(root, rest):
            groups.setdefault(group, []).append(f)
    tree_root = "d" if ctx.attr.dst_dir else "o"
    return [
        DefaultInfo(files = depset(outputs.values())),
        OutputGroupInfo(**{g: depset(fs) for g, fs in groups.items()}),
        XnuTreeInfo(
            root = roots[tree_root],
            files = by_root.get(tree_root, {}),
            base = {k[2:]: f for k, f in base.items()},
        ),
    ]

_COMPONENTS = ("osfmk", "bsd", "iokit", "libkern", "libsa", "pexpert", "san", "security")

# What a driver's compile can reach (the kernel recipes' -I and -include): XNU's exported headers, the iokit, bsd and
# osfmk configuration directories (option headers, MIG and iig headers), the sanitizer list, and the SDK root's
# usr/local/include and Kernel.framework's PrivateHeaders/platform. Fewer declared inputs: less for each compile's
# sandbox to stage.
_SDK_ROOT_DIRS = ("usr/local/include/", "System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform/")

def _in_sdk(root, rest, parts):
    if root == "d":
        return rest.startswith(_SDK_ROOT_DIRS[0]) or rest.startswith(_SDK_ROOT_DIRS[1])
    if root != "o":
        return False
    if parts[0] in ("EXPORT_HDRS", "san"):
        return True
    return (len(parts) > 3 and parts[0].endswith("_ARM64_BCM2837") and parts[1] in ("iokit", "bsd", "osfmk") and
            parts[0].startswith(parts[2] + "_"))

def _groups(root, rest):
    """The output groups a file of the pass's roots belongs to.

    sdk_headers (what the kernel's compiles may include), export_hdrs[_<comp>], gen_<comp>_hdrs / gen_<comp>_srcs (a
    component's generated files, at make's relative names), config_makefile_<comp> (doconf's Makefile: the
    component's object list).
    """
    parts = rest.split("/")
    name = parts[-1]
    source = name.endswith(".c") or name.endswith(".cpp") or name.endswith(".s")
    out = []

    # doconf's Makefile and its config.<CONFIG>_<ARCH> are not headers (config.h is).
    doconf = name == "Makefile" or (name.startswith("config.") and name[len("config."):].isupper())
    if _in_sdk(root, rest, parts) and not source and not doconf:
        out.append("sdk_headers")
    if root == "o" and parts[0] == "EXPORT_HDRS" and len(parts) > 2:
        out += ["export_hdrs_" + parts[1], "export_hdrs"]
    elif root == "o" and parts[0].endswith("_ARM64_BCM2837") and len(parts) > 2 and parts[1] in _COMPONENTS:
        comp = parts[1]
        if name == "Makefile" and len(parts) == 4:
            out.append("config_makefile_" + comp)
        elif source:
            out.append("gen_{}_srcs".format(comp))
        else:
            out.append("gen_{}_hdrs".format(comp))
    elif root == "o" and parts[0] in _COMPONENTS:
        out.append("gen_{}_hdrs".format(parts[0]))
    return out

xnu_pass = rule(
    implementation = _xnu_pass_impl,
    doc = "One recorded XNU make pass (a table from xnu_record.py), replayed as Bazel actions.",
    attrs = {
        "availability": attr.label(default = "@availability_versions//:all", doc = "the SDK root's base: AvailabilityVersions"),
        "config": attr.label(mandatory = True, executable = True, cfg = "exec", doc = "SETUP/config"),
        "decomment": attr.label(mandatory = True, executable = True, cfg = "exec", doc = "SETUP/decomment"),
        "dst_dir": attr.string(doc = "installhdrs: the DSTROOT (the SDK root), below this package's output directory"),
        "libdispatch": attr.label(default = "@libdispatch//:all", doc = "the SDK root's base: libdispatch"),
        "libplatform": attr.label(default = "@libplatform//:all", doc = "the SDK root's base: libplatform"),
        "obj_dir": attr.string(mandatory = True, doc = "the pass's object root, below this package's output directory"),
        "sdkroot": attr.label(providers = [XnuTreeInfo], doc = "build: the SDK root (KERNEL_SDKROOT)"),
        "srcs": attr.label_list(allow_files = True, doc = "every file of the XNU tree (xnu_sources)"),
        "table": attr.string(mandatory = True, doc = "the table, json.encode()d"),
        "xnu_root": attr.string(mandatory = True, doc = "the XNU tree's directory (kernel/xnu-<version>)"),
        "_ops": attr.label(default = "//kernel/rules:xnu_ops.py", allow_single_file = True),
        "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
    },
    toolchains = [PY_TOOLCHAIN],
)

# --- Host tools from XNU's SETUP/ --------------------------------------------------------------------------------
def _xnu_host_tool_impl(ctx):
    # Below bin/: a tool's name (config) can be a component's subpackage (config/), whose outputs share that path.
    out = ctx.actions.declare_file("bin/" + ctx.label.name)
    gen = ctx.actions.declare_directory("bin/" + ctx.label.name + ".gen") if (ctx.file.yacc or ctx.file.lex) else None
    srcs = [f.path for f in ctx.files.srcs]
    lines = ["set -e", "export PATH=/usr/bin:/bin DEVELOPER_DIR={}".format(DEVELOPER_DIR)]
    includes = ["-I" + d for d in sorted({f.dirname: True for f in ctx.files.srcs + ctx.files.hdrs}.keys())]
    if gen:
        lines.append("mkdir -p {}".format(gen.path))
        includes.append("-I" + gen.path)
        if ctx.file.yacc:
            lines.append("{tc}/bison -y -d -d -o {g}/parser.c {y}".format(tc = _TOOLCHAIN, g = gen.path, y = ctx.file.yacc.path))
            srcs.append(gen.path + "/parser.c")
        if ctx.file.lex:
            lines.append("env M4={tc}/gm4 {tc}/flex --header-file={g}/lexer.yy.h -o {g}/lexer.yy.c {l}".format(
                tc = _TOOLCHAIN,
                g = gen.path,
                l = ctx.file.lex.path,
            ))
            srcs.append(gen.path + "/lexer.yy.c")

    # No debug information: the tool's bytes (part of every action key that runs it) name no sandbox path. -Wl,-O1:
    # given the driver's -O0, Apple's ld writes a random LC_UUID, so every build of the tool had other bytes.
    lines.append(" ".join([_TOOLCHAIN + "/clang", "-isysroot", MACOSX_SDK, "-mmacosx-version-min=14.0", "-O0", "-g0"] +
                          ctx.attr.copts + includes + srcs + ["-o", out.path, "-Wl,-O1"] + ctx.attr.linkopts))
    ctx.actions.run_shell(
        command = "\n".join(lines),
        inputs = ctx.files.srcs + ctx.files.hdrs + ctx.files.yacc + ctx.files.lex + [ctx.file._xcode_identity],
        outputs = [out] + ([gen] if gen else []),
        mnemonic = "XnuHostTool",
        progress_message = "Building XNU's host tool " + ctx.label.name,
        execution_requirements = XCODE_EXECUTION,
    )
    return [DefaultInfo(executable = out, files = depset([out]))]

xnu_host_tool = rule(
    implementation = _xnu_host_tool_impl,
    doc = "A host (macOS arm64) tool from XNU's SETUP/, built as its SETUP Makefile does (bison/flex for config).",
    attrs = {
        "copts": attr.string_list(),
        "hdrs": attr.label_list(allow_files = [".h"]),
        "lex": attr.label(allow_single_file = [".l"]),
        "linkopts": attr.string_list(),
        "srcs": attr.label_list(allow_files = [".c"], mandatory = True),
        "yacc": attr.label(allow_single_file = [".y"]),
        "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
    },
    executable = True,
)

# --- The SDK for the kernel's compiles ----------------------------------------------------------------------------
def _one(target):
    return target[0] if type(target) == "list" else target

def _xnu_sdk_impl(ctx):
    obj = _one(ctx.attr.obj)
    sdkroot = _one(ctx.attr.sdkroot)
    headers = obj[OutputGroupInfo].sdk_headers
    sdk_files = sdkroot[OutputGroupInfo].sdk_headers
    source = [f for f in ctx.files.xnu_source if f.extension in ("h", "hpp", "inc", "def")]
    obj_tree = obj[XnuTreeInfo]
    bin_dir = obj_tree.root[:-len("/" + ctx.attr.package_dir + "/" + ctx.attr.obj_dir)]
    context = cc_common.create_compilation_context(
        headers = depset(source + ctx.files.extra, transitive = [headers, sdk_files]),
    )
    sdk = KernelSdkInfo(
        headers = depset(transitive = [headers, sdk_files]),
        obj_dir = obj_tree.root,
        kernel_config = ctx.attr.kernel_config,
        sdkroot_dir = sdkroot[XnuTreeInfo].root,
        objroot = "{}/{}/build/xnu/ios-obj".format(bin_dir, OLD_TREE),
        fakeroot = "{}/{}/.cache/fakeroot".format(bin_dir, OLD_TREE),
        bin_dir = bin_dir,
        xnu_root = ctx.attr.xnu_root,
        tree = TREE_ROOT,
        package = ctx.attr.kernel_package,
    )
    variables = platform_common.TemplateVariableInfo({
        "KERNEL_BUILD": "{}/{}/build".format(bin_dir, ctx.attr.kernel_package),
        "KERNEL_XNU": ctx.attr.xnu_root,
    })
    return [sdk, variables, CcInfo(compilation_context = context), DefaultInfo(files = sdk.headers)]

xnu_sdk = rule(
    implementation = _xnu_sdk_impl,
    doc = "The kernel SDK of one XNU for the kernel configuration: its object root's and SDK root's headers (built in " +
          "the outer configuration, once) and its source's headers, as CcInfo, and the paths the kernel toolchain names.",
    attrs = {
        "extra": attr.label_list(
            allow_files = True,
            doc = "headers XNU's own flags put on every kernel compile (-I <tree>/diagnostics/switch)",
        ),
        "kernel_config": attr.string(mandatory = True, doc = "DEVELOPMENT or RELEASE: the configuration obj is for"),
        "kernel_package": attr.string(mandatory = True, doc = "the package of the kernel's other generated files"),
        "obj": attr.label(mandatory = True, providers = [XnuTreeInfo], cfg = outer_transition),
        "obj_dir": attr.string(mandatory = True),
        "package_dir": attr.string(mandatory = True, doc = "the package of obj (kernel/xnu-<version>)"),
        "sdkroot": attr.label(mandatory = True, providers = [XnuTreeInfo], cfg = outer_transition),
        "xnu_root": attr.string(mandatory = True),
        "xnu_source": attr.label_list(allow_files = True),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)

# --- One directory, for the tools that still take one -----------------------------------------------------------
def _xnu_tree_impl(ctx):
    tree = ctx.attr.src[XnuTreeInfo]
    out = ctx.actions.declare_directory(ctx.attr.out)
    files = sorted(tree.files.values(), key = lambda f: f.path)
    ctx.actions.run_shell(
        inputs = files,
        outputs = [out],
        arguments = [tree.root, out.path],
        # -L: Kernel.framework's alias links become copies of what they name (a tree artifact whose links dangle after
        # a disk-cache hit makes its readers rerun).
        # The .keep markers (empty directories the compiles' -I name) stay out: make's tree had none.
        command = 'cd "$1" && /usr/bin/find -L . -type f ! -name .keep -print0 | /usr/bin/cpio -0pdL --quiet "$OLDPWD/$2"',
        mnemonic = "XnuTree",
        progress_message = "Collecting {} into one directory".format(ctx.attr.src.label),
    )
    return [DefaultInfo(files = depset([out]))]

xnu_tree = rule(
    implementation = _xnu_tree_impl,
    doc = "An xnu_pass's files as one tree artifact (for tools that take a directory: make, xcodebuild).",
    attrs = {
        "out": attr.string(mandatory = True),
        "src": attr.label(mandatory = True, providers = [XnuTreeInfo]),
    },
)

# --- Files out of one directory -----------------------------------------------------------------------------------
def _xnu_extract_impl(ctx):
    tree = ctx.file.src
    outs = [ctx.actions.declare_file(ctx.attr.out_dir + "/" + f) for f in ctx.attr.files]
    ctx.actions.run_shell(
        inputs = [tree],
        outputs = outs,
        arguments = [tree.path] + [o.path for o in outs] + ["--"] + ctx.attr.files,
        command = """set -e
src=$1; shift
outs=""; while [ "$1" != "--" ]; do outs="$outs $1"; shift; done; shift
for o in $outs; do /bin/cp "$src/$1" "$o"; shift; done
""",
        mnemonic = "XnuExtract",
        progress_message = "Taking {} file(s) out of {}".format(len(outs), ctx.attr.src.label),
    )
    return [DefaultInfo(files = depset(outs))]

xnu_extract = rule(
    implementation = _xnu_extract_impl,
    doc = "Files of a tree artifact (a tool's output directory) as files of their own, below out_dir.",
    attrs = {
        "files": attr.string_list(mandatory = True),
        "out_dir": attr.string(mandatory = True),
        "src": attr.label(mandatory = True, allow_single_file = True),
    },
)
