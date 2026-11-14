"""The rlibs rules_rust's bootstrap compile writes for a crate built without process_wrapper, as NVK's Rust builds them."""

load("@rules_rust//rust:rust_common.bzl", "CrateInfo")
load("//third_party/mesa/bazel:defs.bzl", "mesa_platform")

def _nvk_bootstrap_rlibs_impl(ctx):
    crate = ctx.attr.crate[0][CrateInfo]
    rlibs = [dep.crate_info.output for dep in crate.deps.to_list() if dep.crate_info]
    return [DefaultInfo(files = depset(rlibs))]

nvk_bootstrap_rlibs = rule(
    implementation = _nvk_bootstrap_rlibs_impl,
    attrs = {
        "crate": attr.label(mandatory = True, cfg = mesa_platform, providers = [CrateInfo]),
        "platform": attr.string(default = "macos", doc = "mesa_platform's: the Mac, as NVK's build tools are"),
    },
    doc = "The rlibs `crate` depends on, compiled by Rust 1.96.1 (rust-src in its sysroot) like NVK's crates.",
)
