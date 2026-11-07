"""A toolchain that provides nothing."""

def _none_toolchain_impl(_ctx):
    return [platform_common.ToolchainInfo()]

none_toolchain = rule(
    implementation = _none_toolchain_impl,
    doc = "A toolchain that provides nothing, for a toolchain type a rule asks for and never uses.",
)
