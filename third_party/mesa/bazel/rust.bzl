"""NVK's Rust (NAK, NIL, NVK's own, and the crates they use) on rules_rust, with meson's rustc arguments for each crate.

Every crate builds with Rust 1.96.1 (mesa_platform sets //bazel/rust:toolchain_version; @nvk_rust's toolchains) and
keeps meson's -C metadata (rules_rust then adds none of its own: bazel/rust/rules_rust-own-metadata.patch), so its
symbols, and the dylib's, are meson's. Panic locations keep meson's spelling: a source is ../mesa/<path>, a generated
file <path> (as it sat in meson's build directory), the standard library's code instantiated here the rustup path main's
dylib was built with.
"""

load("@rules_rust//rust:defs.bzl", "rust_library", "rust_proc_macro", "rust_static_library")

# meson.build's rust_global_args and nouveau_rust_args (lint levels only; kept as meson passes them).
RUST_GLOBAL_ARGS = [
    "-Aclippy::collapsible_else_if",
    "-Aclippy::needless_lifetimes",
    "-Aclippy::question_mark",
]

NOUVEAU_RUST_ARGS = RUST_GLOBAL_ARGS + [
    "-Dboxed_slice_into_iter",
    "-Ddeprecated_safe_2024",
    "-Dimpl_trait_overcaptures",
    "-Dkeyword_idents_2024",
    "-Dmissing_unsafe_on_extern",
    "-Dnever_type_fallback_flowing_into_unsafe",
    "-Drust_2024_guarded_string_incompatible_syntax",
    "-Drust_2024_incompatible_pat",
    "-Drust_2024_prelude_collisions",
    "-Dstatic_mut_refs",
    "-Dunsafe_op_in_unsafe_fn",
    "-Aclippy::identity_op",
    "-Aclippy::len_zero",
    "-Aclippy::manual_range_contains",
    "-Aclippy::needless_range_loop",
    "-Aclippy::redundant_field_names",
    "-Aclippy::upper_case_acronyms",
    "-Aclippy::vec_box",
    "-Aclippy::write_with_newline",
    "-Aclippy::not_unsafe_ptr_arg_deref",
    "-Anon_snake_case",
]

# meson's buildtype=release (and b_ndebug) arguments for every crate; release adds fat LTO.
_BUILDTYPE = [
    "-Cdebug-assertions=no",
    "-Coverflow-checks=no",
    "-Copt-level=3",
    # meson strips nothing (rules_rust's opt and exec configurations would: rustc's strip leaves the proc-macro dylibs
    # with a misaligned string pool that dlopen refuses).
    "-Cstrip=none",
]

_TARGET_CPU = select({
    "//third_party/mesa/bazel:ios": ["-Ctarget-cpu=cortex-a57"],
    "//conditions:default": [],
})

_EMBED = select({
    "//bazel:release": ["-Cembed-bitcode=yes"],
    "//conditions:default": ["-Cembed-bitcode=no"],
})

_LTO = select({
    "//bazel:release": [
        "-Clto=fat",
        "-Ccodegen-units=1",
    ],
    "//conditions:default": [],
})

# Where meson's rustc saw each file (rustc applies the last matching prefix map): the standard library's sources in
# the sysroot (rust-src) as the rustup toolchain main's dylib was built with, Mesa's sources as ../mesa/, its generated
# files as build-directory paths.
_REMAP = [
    "--remap-path-prefix=$(RUST_SYSROOT)=/Users/brunostjohn/.rustup/toolchains/stable-aarch64-apple-darwin",
    "--remap-path-prefix=third_party/mesa=../mesa",
    "--remap-path-prefix=$(BINDIR)/third_party/mesa=",
]

_TOOLCHAINS = ["@rules_rust//rust/toolchain:current_rust_toolchain"]

# NVK's crates build only with Rust 1.96.1 (mesa_platform sets it, and it carries into the proc-macros' exec
# configuration); `bazel build //...` in the host configuration skips them.
_RUST_1_96_1 = select({
    "//bazel/rust:toolchain_1.96.1": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

def _flags(metadata, lints, native_libs, cap_lints):
    flags = _BUILDTYPE + ["-Cmetadata=" + metadata]
    if cap_lints:
        flags = flags + ["--cap-lints=allow"]
    return flags + lints + ["-lstatic:-bundle=" + lib for lib in native_libs]

def mesa_rust_library(
        name,
        crate_name,
        metadata,
        edition = "2021",
        lints = [],
        native_libs = [],
        cap_lints = False,
        crate_type = "rlib",
        extra_flags = [],
        **kwargs):
    """One of meson's Rust targets (rust_abi 'rust': an rlib; 'c': a staticlib; rust.proc_macro: a proc-macro).

    Args:
        name: the target.
        crate_name: meson's crate name.
        metadata: meson's -C metadata value (<target id>@@<name>@<kind>).
        edition: the crate's edition.
        lints: lint arguments (RUST_GLOBAL_ARGS, NOUVEAU_RUST_ARGS).
        native_libs: the C archives meson names with -l static:-bundle= (recorded, never bundled).
        cap_lints: --cap-lints allow (meson's subprojects).
        crate_type: "rlib", "staticlib" or "proc-macro".
        extra_flags: other rustc arguments meson gives the crate (--cfg ...).
        **kwargs: srcs, crate_root, deps, proc_macro_deps, crate_features, compile_data.
    """
    flags = _flags(metadata, lints, native_libs, cap_lints) + extra_flags
    if crate_type == "proc-macro":
        # Built for (and run by) the build machine: no LTO, no remapping (nothing of it reaches NVK).
        rust_proc_macro(
            name = name,
            crate_name = crate_name,
            edition = edition,
            rustc_flags = flags + ["-Cembed-bitcode=no"],
            target_compatible_with = _RUST_1_96_1,
            **kwargs
        )
        return
    rule = rust_static_library if crate_type == "staticlib" else rust_library
    rule(
        name = name,
        crate_name = crate_name,
        edition = edition,
        rustc_flags = flags + _TARGET_CPU + _EMBED + (_LTO if crate_type == "staticlib" else []) + _REMAP,
        target_compatible_with = _RUST_1_96_1,
        toolchains = _TOOLCHAINS,
        **kwargs
    )
    if crate_type == "staticlib":
        # The same crate as an rlib, which a rust_test takes as its crate (nak_sm53_half_test).
        rust_library(
            name = name + "_rlib",
            crate_name = crate_name,
            edition = edition,
            rustc_flags = flags + _TARGET_CPU + _EMBED + _REMAP,
            target_compatible_with = _RUST_1_96_1,
            toolchains = _TOOLCHAINS,
            **kwargs
        )
