"""mesa_dylib: NVK's libvulkan_nouveau.dylib, linked as meson links it and then relinked as gpu/nvk/relink-nvk.py did.

  1. link1: meson's link command (clang++, its arguments in its order: archive order is code layout) with
     -undefined dynamic_lookup, as meson leaves it;
  2. stubs: the entry-point tables' weak references nothing defines (Mesa's vk_entrypoints_gen.py --weak; ~2960
     nvk_/vk_/wsi_ names, `nm -m`: "(undefined) ... (dynamically looked up)") become absolute private symbols at 0,
     resolved by the static linker instead of a flat-namespace lookup dyld makes at every dlopen in every image;
  3. link2: the same command with the stubs object right after -o and without -undefined dynamic_lookup, so any other
     undefined symbol fails the link. That is the dylib.

Both links run in $(BINDIR)/third_party/mesa (meson's build directory, as Bazel lays it out) with -oso_prefix ., so
the debug map names the Rust staticlib's members as meson's did (src/nouveau/vulkan/libnvk_rs.a(...)), and every
argument is relative to it: the action does not depend on where the execution root is.
"""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@xcode//:defs.bzl", "XCODE_TOOLCHAIN")
load("//kernel/rules:common.bzl", "XCODE_EXECUTION")
load(":defs.bzl", "MESA_ONLY")

_MESA = "third_party/mesa"

# The stub object's target: the dylib's own platform and minimum OS (relink-nvk read them back with vtool).
_STUB_TRIPLE = {
    "ios": "arm64-apple-ios17.0",
    "macos": "arm64-apple-macos27.0",
}

def _archive(target):
    archives = [f for f in target[DefaultInfo].files.to_list() if f.extension == "a"]
    if len(archives) != 1:
        fail("{} provides {} archives, expected one".format(target.label, len(archives)))
    return archives[0]

def _mesa_dylib_impl(ctx):
    cc_toolchain = find_cc_toolchain(ctx)
    features = cc_common.configure_features(ctx = ctx, cc_toolchain = cc_toolchain)
    action = ACTION_NAMES.cpp_link_dynamic_library
    clangxx = cc_common.get_tool_for_action(feature_configuration = features, action_name = action)
    env = cc_common.get_environment_variables(feature_configuration = features, action_name = action, variables = cc_common.empty_variables())
    target_flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = features,
        action_name = action,
        variables = cc_common.empty_variables(),
    )

    root = ctx.bin_dir.path + "/" + _MESA
    up = "/".join([".."] * len(root.split("/")))

    def rel(f):
        if f.path.startswith(root + "/"):
            return f.path[len(root) + 1:]
        return up + "/" + f.path

    out = ctx.actions.declare_file(ctx.attr.out)
    link1 = ctx.actions.declare_file("link1/" + ctx.attr.out)
    stubs_s = ctx.actions.declare_file(ctx.attr.stubs + ".s")
    stubs_o = ctx.actions.declare_file(ctx.attr.stubs + ".o")
    force_load = _archive(ctx.attr.force_load)
    archives = [_archive(t) for t in ctx.attr.archives]
    inputs = depset([force_load] + archives, transitive = [cc_toolchain.all_files])

    def link(output, extra_first, undefined):
        args = ctx.actions.args()
        args.add(root)
        args.add(clangxx)
        args.add_all(target_flags)
        args.add_all(["-o", rel(output)])
        args.add_all([rel(f) for f in extra_first])
        args.add_all(ctx.attr.link_args)
        args.add_all(["-Wl,-force_load", rel(force_load), "-Wl,-oso_prefix,."])
        args.add_all([rel(f) for f in archives])
        if undefined:
            args.add("-Wl,-undefined,dynamic_lookup")
        args.add_all(ctx.attr.libs)
        return args

    ctx.actions.run_shell(
        command = 'cd "$1"; shift; exec "$@"',
        arguments = [link(link1, [], True)],
        inputs = inputs,
        outputs = [link1],
        env = env,
        mnemonic = "MesaLink",
        progress_message = "Linking %{output} (meson's link, flat lookups left)",
        execution_requirements = XCODE_EXECUTION,
    )
    ctx.actions.run_shell(
        command = """\
set -e
"$1" -m "$2" | awk '/\\(undefined\\)/ && /\\(dynamically looked up\\)$/ {print $(NF-3)}' > "$3.names"
if grep -v -E '^_(nvk|vk|wsi)_[A-Za-z0-9_]+$' "$3.names" >&2; then
  echo "mesa_dylib: the flat-namespace lookups above are not entry-point table names" >&2
  exit 1
fi
awk '{printf ".private_extern %s\\n%s = 0\\n", $1, $1}' "$3.names" > "$3"
rm "$3.names"
""",
        arguments = [XCODE_TOOLCHAIN + "/nm", link1.path, stubs_s.path],
        inputs = depset([link1], transitive = [cc_toolchain.all_files]),
        outputs = [stubs_s],
        env = env,
        mnemonic = "MesaAbsentEntrypoints",
        progress_message = "Listing the absent entry points of %{input}",
        execution_requirements = XCODE_EXECUTION,
    )
    asm = ctx.actions.args()
    asm.add_all(["-c", "-target", _STUB_TRIPLE[ctx.attr.platform], stubs_s, "-o", stubs_o])
    ctx.actions.run(
        executable = XCODE_TOOLCHAIN + "/clang",
        arguments = [asm],
        inputs = depset([stubs_s], transitive = [cc_toolchain.all_files]),
        outputs = [stubs_o],
        env = env,
        mnemonic = "MesaAbsentEntrypointsObject",
        progress_message = "Assembling %{output}",
        execution_requirements = XCODE_EXECUTION,
    )
    ctx.actions.run_shell(
        command = 'cd "$1"; shift; exec "$@"',
        arguments = [link(out, [stubs_o], False)],
        inputs = depset([stubs_o], transitive = [inputs]),
        outputs = [out],
        env = env,
        mnemonic = "MesaLink",
        progress_message = "Linking %{output}",
        execution_requirements = XCODE_EXECUTION,
    )
    return [DefaultInfo(files = depset([out]))]

_mesa_dylib = rule(
    implementation = _mesa_dylib_impl,
    doc = "A Mesa shared library linked like meson links it, its absent weak entry points resolved to 0 (see above).",
    attrs = {
        "archives": attr.label_list(mandatory = True, doc = "the other archives, in meson's order"),
        "force_load": attr.label(mandatory = True, doc = "the archive meson links whole (-Wl,-force_load)"),
        "libs": attr.string_list(doc = "meson's arguments after the archives (system libraries and frameworks)"),
        "link_args": attr.string_list(doc = "meson's arguments between -o and the archives"),
        "out": attr.string(mandatory = True, doc = "the dylib's name (as meson's)"),
        "platform": attr.string(mandatory = True, values = _STUB_TRIPLE.keys()),
        "stubs": attr.string(mandatory = True, doc = "the absent entry points' object, without extension"),
    },
    toolchains = use_cc_toolchain(),
    fragments = ["cpp"],
)

def mesa_dylib(**kwargs):
    """A Mesa dylib (see _mesa_dylib), built only in NVK's configuration.

    Args:
        **kwargs: the rule's attributes.
    """
    _mesa_dylib(target_compatible_with = MESA_ONLY, **kwargs)

def _mesa_relocatable_impl(ctx):
    cc_toolchain = find_cc_toolchain(ctx)
    features = cc_common.configure_features(ctx = ctx, cc_toolchain = cc_toolchain)
    action = ACTION_NAMES.cpp_link_dynamic_library
    clangxx = cc_common.get_tool_for_action(feature_configuration = features, action_name = action)
    env = cc_common.get_environment_variables(
        feature_configuration = features,
        action_name = action,
        variables = cc_common.empty_variables(),
    )
    target_flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = features,
        action_name = action,
        variables = cc_common.empty_variables(),
    )
    out = ctx.actions.declare_file(ctx.attr.out)
    link1 = ctx.actions.declare_file("link1/" + ctx.attr.out)
    stubs_s = ctx.actions.declare_file(ctx.attr.name + "_absent.s")
    stubs_o = ctx.actions.declare_file(ctx.attr.name + "_absent.o")
    exported = ctx.actions.declare_file(ctx.attr.name + ".exported")
    ctx.actions.write(exported, "".join(["_{}\n".format(s) for s in ctx.attr.exported]))
    force_load = _archive(ctx.attr.force_load)
    archives = [_archive(t) for t in ctx.attr.archives]

    def driver(arguments, inputs, output, mnemonic, message):
        args = ctx.actions.args()
        args.add_all(target_flags)
        args.add_all(arguments)
        ctx.actions.run(
            executable = clangxx,
            arguments = [args],
            inputs = depset(inputs, transitive = [cc_toolchain.all_files]),
            outputs = [output],
            env = env,
            mnemonic = mnemonic,
            progress_message = message,
            execution_requirements = XCODE_EXECUTION,
        )

    driver(
        ["-r", "-nostdlib", "-o", link1.path, "-Wl,-force_load," + force_load.path] + [a.path for a in archives],
        [force_load] + archives,
        link1,
        "MesaRelocatable",
        "Linking %{output}",
    )

    # The runtime's entry-point tables' weak references nothing defines, as mesa_dylib resolves them: private at 0
    ctx.actions.run_shell(
        command = """\
set -e
"$1" -m -u "$2" | awk '/\\(undefined\\) weak external/ {print $NF}' > "$3.names"
if grep -v -E '^_(nvk|vk|wsi)_[A-Za-z0-9_]+$' "$3.names" >&2; then
  echo "mesa_relocatable: the weak references above are not entry-point table names" >&2
  exit 1
fi
awk '{printf ".private_extern %s\\n%s = 0\\n", $1, $1}' "$3.names" > "$3"
rm "$3.names"
""",
        arguments = [XCODE_TOOLCHAIN + "/nm", link1.path, stubs_s.path],
        inputs = depset([link1], transitive = [cc_toolchain.all_files]),
        outputs = [stubs_s],
        env = env,
        mnemonic = "MesaAbsentEntrypoints",
        progress_message = "Listing the absent entry points of %{input}",
        execution_requirements = XCODE_EXECUTION,
    )
    driver(["-c", "-x", "assembler", stubs_s.path, "-o", stubs_o.path], [stubs_s], stubs_o, "MesaAbsentEntrypointsObject", "Assembling %{output}")
    driver(
        ["-r", "-nostdlib", "-o", out.path, "-Wl,-exported_symbols_list," + exported.path, link1.path, stubs_o.path],
        [exported, link1, stubs_o],
        out,
        "MesaRelocatable",
        "Linking %{output} (its entry points global, the rest local)",
    )
    return [DefaultInfo(files = depset([out]))]

_mesa_relocatable = rule(
    implementation = _mesa_relocatable_impl,
    doc = """One relocatable object (ld -r) of an archive linked whole and what it needs of other archives, whose only
    global symbols are `exported`: every other one, a Rust staticlib's standard library included, is local to it.""",
    attrs = {
        "archives": attr.label_list(mandatory = True, doc = "the archives `force_load` draws members from"),
        "exported": attr.string_list(mandatory = True, doc = "the C names left global"),
        "force_load": attr.label(mandatory = True, doc = "the archive linked whole"),
        "out": attr.string(mandatory = True),
    },
    toolchains = use_cc_toolchain(),
    fragments = ["cpp"],
)

def mesa_relocatable(**kwargs):
    """A Mesa relocatable object (see _mesa_relocatable), built only in NVK's configuration.

    Args:
        **kwargs: the rule's attributes.
    """
    _mesa_relocatable(target_compatible_with = MESA_ONLY, **kwargs)
