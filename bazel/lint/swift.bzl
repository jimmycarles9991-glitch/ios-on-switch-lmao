"""The repository's Swift files as declared by lint_files(), without scanning working-copy directories."""

def _swift_sources_impl(ctx):
    return [DefaultInfo(files = depset([f for f in ctx.files.srcs if f.extension == "swift"]))]

swift_sources = rule(
    implementation = _swift_sources_impl,
    attrs = {"srcs": attr.label_list(allow_files = True, mandatory = True)},
)
