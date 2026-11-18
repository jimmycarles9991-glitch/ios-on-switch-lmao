"""Source and release payload pins for the rootless bootstrap."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")

_BUILD = """\
package(default_visibility = ["//visibility:public"])
exports_files(glob(["**"]))
filegroup(name = "source_tree", srcs = glob(["**"]))
"""

def _dependencies_impl(ctx):
    pins = json.decode(ctx.read(Label("//bootstrap:sources.json")))
    for source in pins["sources"]:
        http_archive(
            name = "bootstrap_" + source["name"],
            urls = [source["url"]],
            sha256 = source["sha256"],
            strip_prefix = source["prefix"],
            build_file_content = _BUILD,
        )
    for artifact in pins["files"]:
        http_file(
            name = "bootstrap_" + artifact["name"],
            urls = [artifact["url"]],
            sha256 = artifact["sha256"],
        )

bootstrap_dependencies = module_extension(implementation = _dependencies_impl)
