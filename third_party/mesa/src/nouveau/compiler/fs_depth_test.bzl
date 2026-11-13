"""Link the NIR interpolation test with Mesa's configured archive toolchain."""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("//kernel/rules:common.bzl", "XCODE_EXECUTION")

def _fs_depth_test_binary_impl(ctx):
    toolchain = find_cc_toolchain(ctx)
    features = cc_common.configure_features(ctx = ctx, cc_toolchain = toolchain)
    action = ACTION_NAMES.cpp_link_executable
    variables = cc_common.empty_variables()
    archives = []
    for target in ctx.attr.archives:
        files = [f for f in target[DefaultInfo].files.to_list() if f.extension == "a"]
        if len(files) != 1:
            fail("{} must provide one archive".format(target.label))
        archives.extend(files)
    output = ctx.actions.declare_file(ctx.label.name)
    args = ctx.actions.args()
    args.add_all(cc_common.get_memory_inefficient_command_line(
        feature_configuration = features,
        action_name = action,
        variables = variables,
    ))
    args.add_all(["-o", output])
    args.add_all(archives)
    args.add_all(["-lc++", "-lz"])
    args.add_all(ctx.attr.linkopts)
    ctx.actions.run(
        executable = cc_common.get_tool_for_action(feature_configuration = features, action_name = action),
        arguments = [args],
        inputs = depset(archives, transitive = [toolchain.all_files]),
        outputs = [output],
        env = cc_common.get_environment_variables(feature_configuration = features, action_name = action, variables = variables),
        execution_requirements = XCODE_EXECUTION,
        mnemonic = "NakInterpolationTestLink",
    )
    return [DefaultInfo(executable = output)]

fs_depth_test_binary = rule(
    implementation = _fs_depth_test_binary_impl,
    attrs = {
        "archives": attr.label_list(mandatory = True),
        "linkopts": attr.string_list(),
    },
    executable = True,
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
