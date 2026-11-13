"""Objective-C++ units for host tests: cc rules take no .mm source, so each one is compiled through a .cpp of its own that
includes it (with `-x objective-c++` in the library's copts, and the .mm files among its additional_compiler_inputs)."""

def _objcxx_units_impl(ctx):
    units = []
    for src in ctx.files.srcs:
        unit = ctx.actions.declare_file("{}/{}.cpp".format(ctx.label.name, src.basename))
        ctx.actions.write(unit, '#include "{}"\n'.format(src.path))
        units.append(unit)
    return [DefaultInfo(files = depset(units))]

objcxx_units = rule(
    implementation = _objcxx_units_impl,
    doc = "One <basename>.cpp per Objective-C++ source, including it by its execution-root path.",
    attrs = {"srcs": attr.label_list(allow_files = [".mm"], mandatory = True)},
)
