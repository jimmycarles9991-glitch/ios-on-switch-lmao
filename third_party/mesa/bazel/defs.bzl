"""Mesa's NVK build in Bazel, the way meson builds it (third_party/mesa/meson.build and its subdirectories):

  mesa_cc_library   one meson static_library's objects: cc_library with exactly meson's compile flags for that target
                    (mesa_args), one action per source, so an edit recompiles that file only
  mesa_archive      a meson .a: the objects of mesa_cc_library targets in meson's member order (link order is code
                    layout), archived by the toolchain's ar
  mesa_platform     the transition into the configuration NVK builds in: the iOS or macOS user platform and Rust 1.96.1
  cargo_tool        bindgen and cbindgen (host tools, from their own Cargo.lock)

Paths: Mesa's sources are third_party/mesa/<path>; what meson writes to its build directory, Bazel writes to
$(BINDIR)/third_party/mesa/<the same path> (every generator's outputs are declared in the package of the meson.build
that makes them), so meson's include directories translate one for one. Strings that reach the binary keep meson's
spelling: __FILE__ of a source is ../mesa/<path> and of a generated file <path> (-fmacro-prefix-map), as in the
meson build directory next to the source tree that main's dylibs were built in.
"""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//kernel/rules:common.bzl", "XCODE_EXECUTION")
load(":flags.bzl", "MESA_CPP_ARGS", "MESA_C_ARGS", "MESA_PRE_ARGS")

MESA = "third_party/mesa"

# Mesa's targets only build in NVK's configuration (mesa_platform: a :user platform); `bazel build //...` in the host
# configuration skips them. Its tools (the generators' py_binary targets, the proc-macros) build for the host.
MESA_ONLY = ["//bazel/platforms:user"]

# Mesa's own files in a package: never ios-on-switch-lmao's to format or lint (lint_files(dirs = [], exclude = ...)).
MESA_LINT_EXCLUDE = [
    "*.c",
    "*.cc",
    "*.cpp",
    "*.h",
    "*.hpp",
    "*.inc",
    "*.json",
    "*.m",
    "*.md",
    "*.mm",
    "*.py",
    "*.rs",
    "*.sh",
    "*.toml",
    ".clang-format",
    ".clang-tidy",
]

# What the generators' Python imports beyond the standard library.
MESA_PY_DEPS = [
    "@switch_pypi//mako",
    "@switch_pypi//pyyaml",
]

# meson's buildtype and b_ndebug arguments, which it puts after a target's include directories and visibility: the
# debug profile is today's NVK (buildtype=release, b_ndebug=true, no LTO; //bazel:profile); release adds full LTO.
_HEAD = {
    "c": ["-DNDEBUG", "-Wall", "-Winvalid-pch", "-std=c11", "-O3"],
    "cxx": ["-DNDEBUG", "-Wall", "-Winvalid-pch", "-std=c++17", "-O3"],
}

_PROFILE = select({
    "//bazel:release": ["-flto"],
    "//conditions:default": [],
})

# The iOS cross file's c_args (after the project's c_args).
_CROSS_C = select({
    "//third_party/mesa/bazel:ios": ["-Wno-error=incompatible-function-pointer-types"],
    "//conditions:default": [],
})

# __FILE__ as meson's build directory spelled it.
_PREFIX_MAPS = [
    "-fmacro-prefix-map={}/=../mesa/".format(MESA),
    "-fmacro-prefix-map=$(BINDIR)/{}/=".format(MESA),
]

def _shell(flag):
    """copts are Bourne-tokenized: quote a flag that carries quotes (-DPACKAGE_VERSION="27.0.0-devel")."""
    if '"' in flag or "'" in flag or " " in flag:
        return "'" + flag.replace("'", "'\\''") + "'"
    return flag

def _include(directory):
    """The -I for one of meson's include directories.

    Args:
        directory: "src/util" a source directory, "gen:src/util" a build one ("gen:" the build root), "." Mesa's
            root, "execroot:" the execution root.

    Returns:
        The flag.
    """
    if directory == "execroot:":
        return "-I."
    if directory.startswith("gen:"):
        path = directory[len("gen:"):]
        return "-I$(BINDIR)/{}{}".format(MESA, "/" + path if path else "")
    return "-I{}{}".format(MESA, "" if directory == "." else "/" + directory)

def mesa_args(lang, include_dirs, hidden = True, pre = True, proj = True, extra = []):
    """One meson target's compile arguments for one language, in meson's order.

    Args:
        lang: "c" or "cxx".
        include_dirs: the target's include directories in meson's order (see _include; not `includes`, which
            buildifier would sort).
        hidden: gnu_symbol_visibility: 'hidden' (-fvisibility=hidden).
        pre: the project's pre_args (MESA_PRE_ARGS).
        proj: the project's c_args/cpp_args (MESA_C_ARGS / MESA_CPP_ARGS) and, for C on iOS, the cross file's c_args.
        extra: the target's own arguments (c_args/cpp_args, its dependencies' compile_args), or a dict of them by
            platform ("ios", "macos") where the two builds differ.

    Returns:
        The arguments, a list (or a list with select()s).
    """
    head = [_include(d) for d in include_dirs]
    if hidden:
        head.append("-fvisibility=hidden")
    args = [_shell(f) for f in head + _HEAD[lang]] + _PROFILE
    if pre:
        args = args + [_shell(f) for f in MESA_PRE_ARGS]
    if proj:
        args = args + (MESA_C_ARGS if lang == "c" else MESA_CPP_ARGS)
        if lang == "c":
            args = args + _CROSS_C
    if type(extra) == "dict":
        extra = select({
            "//third_party/mesa/bazel:ios": [_shell(f) for f in extra["ios"]],
            "//conditions:default": [_shell(f) for f in extra["macos"]],
        })
    else:
        extra = [_shell(f) for f in extra]
    return args + extra + _PREFIX_MAPS

def mesa_cc_library(name, sources, c_args = [], cpp_args = [], deps = [], **kwargs):
    """One of meson's static_library targets: its objects, one action each, and its archive.

    Every Mesa header is an input, pruned per object by its .d file: a header edit recompiles only the objects that
    include it.

    Args:
        name: meson's library name (the archive is lib<name>.a).
        sources: its sources in meson's archive member order, which is link order (`sources`, not `srcs`, which
            buildifier would sort).
        c_args: mesa_args("c", ...).
        cpp_args: mesa_args("cxx", ...).
        deps: headers of its own package that //third_party/mesa:headers leaves out.
        **kwargs: other cc_library attributes.
    """
    cc_library(
        name = name,
        srcs = sources,
        conlyopts = c_args,
        cxxopts = cpp_args,
        deps = ["//third_party/mesa:headers"] + deps,
        linkstatic = True,
        # Mesa's own code: never ios-on-switch-lmao's to lint (clang-tidy skips no-lint targets).
        tags = ["no-lint"],
        target_compatible_with = MESA_ONLY,
        **kwargs
    )

def mesa_genrule(**kwargs):
    """A generator of one of Mesa's meson.build files (genrule), built only in NVK's configuration.

    Args:
        **kwargs: the genrule's attributes.
    """
    native.genrule(target_compatible_with = MESA_ONLY, **kwargs)

def _objects(target):
    objects = []
    for linker_input in target[CcInfo].linking_context.linker_inputs.to_list():
        if linker_input.owner != target.label:
            continue
        for library in linker_input.libraries:
            objects.extend(library.objects)
    return objects

def _mesa_archive_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.out)
    objects = []
    for lib in ctx.attr.libs:
        objects.extend(_objects(lib))
    cc_toolchain = find_cc_toolchain(ctx)
    features = cc_common.configure_features(ctx = ctx, cc_toolchain = cc_toolchain)
    ar = cc_common.get_tool_for_action(feature_configuration = features, action_name = ACTION_NAMES.cpp_link_static_library)
    env = cc_common.get_environment_variables(
        feature_configuration = features,
        action_name = ACTION_NAMES.cpp_link_static_library,
        variables = cc_common.empty_variables(),
    )
    args = ctx.actions.args()
    args.add("rcs", out)
    args.add_all(objects)
    ctx.actions.run(
        executable = ar,
        arguments = [args],
        inputs = depset(objects, transitive = [cc_toolchain.all_files]),
        outputs = [out],
        env = env,
        mnemonic = "MesaArchive",
        progress_message = "Archiving %{output}",
        execution_requirements = XCODE_EXECUTION,
    )
    return [DefaultInfo(files = depset([out]))]

_mesa_archive = rule(
    implementation = _mesa_archive_impl,
    doc = "One of meson's .a files: the objects of `libs`, each in its srcs order, in this order (meson's ar order).",
    attrs = {
        "libs": attr.label_list(mandatory = True, providers = [CcInfo], doc = "mesa_cc_library targets, in member order"),
        "out": attr.string(mandatory = True, doc = "the archive's name, as meson's"),
    },
    toolchains = use_cc_toolchain(),
    fragments = ["cpp"],
)

def mesa_archive(**kwargs):
    """A meson .a made of several libraries' objects (see _mesa_archive), built only in NVK's configuration.

    Args:
        **kwargs: name, out, libs.
    """
    _mesa_archive(target_compatible_with = MESA_ONLY, **kwargs)

def _mesa_file_impl(ctx):
    out = ctx.outputs.out
    ctx.actions.run(
        executable = "/bin/cp",
        arguments = [ctx.file.src.path, out.path],
        inputs = [ctx.file.src],
        outputs = [out],
        mnemonic = "MesaFile",
        progress_message = "Copying %{input} to %{output}",
    )
    return [DefaultInfo(files = depset([out]))]

_mesa_file = rule(
    implementation = _mesa_file_impl,
    doc = "`src` copied to `out` in this package's output directory (cacheable, unlike skylib's copy_file).",
    attrs = {
        "out": attr.output(mandatory = True),
        "src": attr.label(mandatory = True, allow_single_file = True),
    },
)

def mesa_file(**kwargs):
    """A file put where meson's build directory has it (see _mesa_file), only in NVK's configuration.

    Args:
        **kwargs: name, src, out.
    """
    _mesa_file(target_compatible_with = MESA_ONLY, **kwargs)

# --- The configuration NVK builds in -----------------------------------------------------------------------------

_PLATFORMS = "//command_line_option:platforms"
_RUST = "//bazel/rust:toolchain_version"

MESA_PLATFORMS = {
    "ios": "//bazel/platforms:ios_arm64",
    "macos": "//bazel/platforms:macos_user_arm64",
}

def _mesa_platform_impl(_settings, attr):
    return {_PLATFORMS: [str(Label(MESA_PLATFORMS[attr.platform]))], _RUST: "1.96.1"}

mesa_platform = transition(implementation = _mesa_platform_impl, inputs = [], outputs = [_PLATFORMS, _RUST])

def _mesa_files_impl(ctx):
    files = depset(transitive = [t[DefaultInfo].files for t in ctx.attr.srcs])
    return [DefaultInfo(files = files, runfiles = ctx.runfiles(transitive_files = files))]

mesa_files = rule(
    implementation = _mesa_files_impl,
    doc = "`srcs` built for `platform` (ios or macos) in NVK's configuration: its files.",
    attrs = {
        "platform": attr.string(mandatory = True, values = MESA_PLATFORMS.keys()),
        "srcs": attr.label_list(cfg = mesa_platform, allow_files = True),
    },
)

def _mesa_make_variables_impl(ctx):
    return [platform_common.TemplateVariableInfo(ctx.attr.variables)]

mesa_make_variables = rule(
    implementation = _mesa_make_variables_impl,
    doc = "Make variables for genrules (toolchains = [...]), chosen by the configuration's platform.",
    attrs = {"variables": attr.string_dict()},
)

# --- Host tools ----------------------------------------------------------------------------------------------------

def _cargo_tool_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name)
    rustc = ctx.file._rustc
    cargo = ctx.file._cargo
    src = ctx.file.manifest.dirname
    vendor = src.rpartition("/")[0] + "/vendor"
    ctx.actions.run_shell(
        command = """\
set -e
here=$PWD
export CARGO_HOME=$here/{out}.cargo-home RUSTC=$here/{rustc} PATH=/usr/bin:/bin
export DEVELOPER_DIR=$(sed -n 's/^DEVELOPER_DIR=//p' {xcode_env})
cd {src}
$here/{cargo} build --release --offline --locked --bin {bin} --target-dir $here/{out}.target \\
  --config 'source.crates-io.replace-with="vendored"' --config "source.vendored.directory=\\"$here/{vendor}\\""
cp $here/{out}.target/release/{bin} $here/{out}
rm -rf $here/{out}.target $here/{out}.cargo-home
""".format(
            out = out.path,
            rustc = rustc.path,
            cargo = cargo.path,
            src = src,
            vendor = vendor,
            bin = ctx.attr.bin,
            xcode_env = ctx.file._xcode_env.path,
        ),
        inputs = ctx.files.srcs + ctx.files._rust + [ctx.file._xcode_env, ctx.file._xcode_identity],
        outputs = [out],
        mnemonic = "CargoTool",
        progress_message = "Building {} with cargo (offline, its own Cargo.lock)".format(ctx.attr.bin),
        execution_requirements = XCODE_EXECUTION,
    )
    return [DefaultInfo(files = depset([out]), executable = out)]

cargo_tool = rule(
    implementation = _cargo_tool_impl,
    doc = "A crates.io binary built by Rust 1.96.1's cargo from a vendored source repository (repos.bzl).",
    executable = True,
    attrs = {
        "bin": attr.string(mandatory = True),
        "manifest": attr.label(mandatory = True, allow_single_file = True, doc = "its src/Cargo.toml"),
        "srcs": attr.label(mandatory = True, doc = "the repository's :all"),
        "_cargo": attr.label(default = "@nvk_rust//:bin/cargo", allow_single_file = True),
        "_rust": attr.label(default = "@nvk_rust//:all"),
        "_rustc": attr.label(default = "@nvk_rust//:bin/rustc", allow_single_file = True),
        "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
        "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
    },
)
