"""mesa_relocatable: one relocatable object (ld -r) of a Mesa archive, its entry points its only global symbols and the
entry-point tables' weak references nothing defines (Mesa's vk_entrypoints_gen.py --weak) private symbols at 0."""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@xcode//:defs.bzl", "XCODE_TOOLCHAIN")
load("//kernel/rules:common.bzl", "XCODE_EXECUTION")
load(":defs.bzl", "MESA_ONLY")

def _archive(target):
    archives = [f for f in target[DefaultInfo].files.to_list() if f.extension == "a"]
    if len(archives) != 1:
        fail("{} provides {} archives, expected one".format(target.label, len(archives)))
    return archives[0]

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

    # -S: ld -r writes a debug map for the Rust standard library's objects (the only members with debug information),
    # naming them by the worker's absolute path to the archive, so each link differed with the worker slot and the
    # output directory, and so did every key hashed from the object (//gpu:stack_key). No debugger can open those paths.
    no_debug_map = ["-Wl,-S"]

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
        ["-r", "-nostdlib"] + no_debug_map + ["-o", link1.path, "-Wl,-force_load," + force_load.path] +
        [a.path for a in archives],
        [force_load] + archives,
        link1,
        "MesaRelocatable",
        "Linking %{output}",
    )

    # The runtime's entry-point tables' weak references nothing defines: private at 0
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
        ["-r", "-nostdlib"] + no_debug_map +
        ["-o", out.path, "-Wl,-exported_symbols_list," + exported.path, link1.path, stubs_o.path],
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
