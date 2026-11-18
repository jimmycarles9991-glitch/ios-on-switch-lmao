"""Replace the device CPU default for QA without appending a duplicate arm64 architecture."""

def _simulator_transition_impl(_settings, _attr):
    return {"//command_line_option:ios_multi_cpus": ["sim_arm64"]}

_simulator_transition = transition(
    implementation = _simulator_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:ios_multi_cpus"],
)

def _simulator_impl(ctx):
    return [DefaultInfo(files = ctx.attr.app[0][DefaultInfo].files)]

ios_simulator = rule(
    implementation = _simulator_impl,
    attrs = {
        "app": attr.label(
            cfg = _simulator_transition,
            mandatory = True,
        ),
    },
)

def _simulator_test_impl(ctx):
    executable = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(
        output = executable,
        target_file = ctx.executable.bundle,
        is_executable = True,
    )
    runfiles = ctx.runfiles(files = [ctx.executable.bundle]).merge(
        ctx.attr.bundle[0][DefaultInfo].default_runfiles,
    )
    return [DefaultInfo(executable = executable, runfiles = runfiles)]

ios_simulator_test = rule(
    implementation = _simulator_test_impl,
    test = True,
    attrs = {
        "bundle": attr.label(
            cfg = _simulator_transition,
            executable = True,
            mandatory = True,
        ),
    },
)
