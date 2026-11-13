"""How the build's Rust is compiled: one rule, `rust_artifact`, builds a rules_rust crate (and every crate below it) for a
platform and a purpose, mapping //bazel:profile onto rustc flags for that crate graph alone.

    rust_artifact(name = "bridge_ios", crate = ":bridge", kind = "device")

kind:
  device       the iPad's code (aarch64-apple-ios, //bazel/platforms:ios_arm64), what the plugin links.
  device_mac   the same code for this Mac (the host simulation links it).
  tool         code the build runs on this Mac (the precompile's host bridge, the corpus runner): optimized and quick to
               build in both profiles; what it computes does not depend on how it was compiled.

Profiles (//bazel:profile, r17-bazel's one switch):
  release  cargo's release profile as the Metal bridge had it: opt-level 3, one codegen unit, line tables (debuginfo 1),
           every rlib as bitcode (-C linker-plugin-lto) and fat LTO at the root staticlib. The release bridge is byte for
           byte the one cargo built (each crate keeps cargo's -C metadata/extra-filename: crates.MODULE.bazel and the
           m2v crates' BUILD files; the source paths are remapped to the ones cargo's build named).
  debug    the default, for edit-build cycles: every crate its own action, no LTO of any kind (not even rustc's
           thin-local LTO between a crate's codegen units), many codegen units, line tables, and our own crates
           (INCREMENTAL_CRATES) compiled incrementally. opt-level 3 with debug assertions and overflow checks off, as in
           release: the device runs this bridge for every app's Metal, and m2v's debug assertions re-derive and compare
           whole bind states on every draw (a CA-shaped frame took 2.7x the CPU of the release bridge at opt-level 1
           with them on).

Nothing here is global: the flags reach rules_rust through its own build settings, set by the transition on this
rule's `crate` only, so other Rust in the build (NVK's NAK, r18-mesa) keeps its own flags.
"""

IOS_PLATFORM = "//bazel/platforms:ios_arm64"

_PLATFORMS = "//command_line_option:platforms"
_PROFILE = "//bazel:profile"
_INCREMENTAL = "//bazel/rust:incremental"
_EXTRA_FLAGS = "@rules_rust//rust/settings:extra_rustc_flags"
_EXTRA_ENV = "@rules_rust//rust/settings:extra_rustc_env"
_PER_CRATE = "@rules_rust//rust/settings:per_crate_rustc_flag"
_LTO = "@rules_rust//rust/settings:lto"
_PIPELINED = "@rules_rust//rust/settings:pipelined_compilation"

# Source paths as rustc records them (panic locations, debug info), the same in both profiles and on every machine:
# the crate roots are execroot-relative, so these name each tree by a fixed prefix (rustc matches whole path components;
# the crates.io crates' are theirs, in crates.MODULE.bazel). They are the paths cargo's build of the bridge gave
# (tools/gpu/cargo_bridge.py laid m2v in as m2v/ next to the bridge's src/), so the release plugin's strings are the same.
REMAP_FLAGS = [
    "--remap-path-prefix=third_party/m2v=m2v",
    "--remap-path-prefix=drivers/NativeMetal/bridge=src",
]

# The deployment targets the plugin and the host tools are built for (rustc reads them from the environment: the
# objects' minimum OS, and the LLVM target triple).
_DEPLOYMENT_ENV = [
    "IPHONEOS_DEPLOYMENT_TARGET=17.0",
    "MACOSX_DEPLOYMENT_TARGET=14.0",
]

# Every target-configured crate of the graph (build scripts and proc-macros, exec-configured, keep rules_rust's own).
_CRATE_FLAGS = {
    ("device", "release"): [
        "-Copt-level=3",
        "-Ccodegen-units=1",
        "-Cdebuginfo=1",
        "-Csplit-debuginfo=unpacked",
        # cargo's OnlyBitcode for every crate below the LTO root: bitcode in the rlib, no object code.
        "-Clinker-plugin-lto",
    ],
    ("device", "debug"): [
        "-Copt-level=3",
        "-Ccodegen-units=64",
        "-Clto=off",
        "-Cembed-bitcode=no",
        # File and line in backtraces, nothing more: m2v's own dev profile (its Cargo.toml: full debug info made each link
        # of the translator 35-47 s); here it also makes the debug bridge 1/3 the size.
        "-Cdebuginfo=line-tables-only",
        "-Csplit-debuginfo=unpacked",
        "-Cdebug-assertions=off",
        "-Coverflow-checks=off",
    ],
    # The precompile's: cargo's release profile less LTO and debug info (what the old build's --quick did), in both profiles.
    ("tool", "release"): [
        "-Copt-level=3",
        "-Ccodegen-units=16",
        "-Clto=off",
        "-Cembed-bitcode=no",
        "-Cdebuginfo=0",
    ],
}
_CRATE_FLAGS[("device_mac", "release")] = _CRATE_FLAGS[("device", "release")]
_CRATE_FLAGS[("device_mac", "debug")] = _CRATE_FLAGS[("device", "debug")]
_CRATE_FLAGS[("tool", "debug")] = _CRATE_FLAGS[("tool", "release")]

# The root crate's own (a staticlib or binary): fat LTO over all of it in release.
_ROOT_FLAGS = {
    ("device", "release"): [
        "-Clinker-plugin-lto=no",
        "-Clto=fat",
    ],
}
_ROOT_FLAGS[("device_mac", "release")] = _ROOT_FLAGS[("device", "release")]

# The debug profile compiles the crates we edit incrementally (rustc's -C incremental; bazel/rust/rules_rust.patch): an
# edit recompiles what it reaches in that crate and in its dependents, not all of them. The sessions live below the output
# base (one per configuration and action kind), outside Bazel's caches; the rlibs are declared outputs as ever. The
# crates.io crates never change and keep the sandbox. 256 codegen units: rustc's own for incremental work (a small unit
# is reused unless an edit reaches it). Release never compiles incrementally.
INCREMENTAL_CRATES = [
    "//third_party/m2v/",
    "//gpu:bridge",
    "//drivers/TegraGM20BMetal/corpus/runner:",
]

_INCREMENTAL_FLAGS = [
    # Below the output base (the action runs unsandboxed, in the execroot <output base>/execroot/_main).
    "-Cincremental=${pwd}/../../rust_incremental",
    "-Ccodegen-units=256",
]

def _filter(label):
    """A rules_rust per-crate filter for exactly `label`'s crate (its label without the repository's @s)."""
    text = str(label)
    for prefix in ("@@//", "@//"):
        if text.startswith(prefix):
            return "//" + text[len(prefix):]
    return text

def _rust_transition_impl(settings, attr):
    key = (attr.kind, settings[_PROFILE])
    return {
        _PLATFORMS: [IOS_PLATFORM] if attr.kind == "device" else settings[_PLATFORMS],
        # Strip nothing whatever the compilation mode (rules_rust strips debug info under -c opt, cargo did not).
        _EXTRA_FLAGS: REMAP_FLAGS + _CRATE_FLAGS[key] + ["-Cstrip=none"],
        _EXTRA_ENV: _DEPLOYMENT_ENV,
        _PER_CRATE: ["{}@{}".format(_filter(attr.crate), flag) for flag in _ROOT_FLAGS.get(key, [])] + (
            ["{}@{}".format(crate, flag) for crate in INCREMENTAL_CRATES for flag in _INCREMENTAL_FLAGS] if settings[_PROFILE] == "debug" and settings[_INCREMENTAL] else []
        ),
        # Each crate's LTO form is the flags' above; rules_rust adds none of its own.
        _LTO: "manual",
        # A crate's dependents start on its metadata (.rmeta) while its code is still being generated.
        _PIPELINED: True,
    }

_rust_transition = transition(
    implementation = _rust_transition_impl,
    inputs = [_PLATFORMS, _PROFILE, _INCREMENTAL],
    outputs = [_PLATFORMS, _EXTRA_FLAGS, _EXTRA_ENV, _PER_CRATE, _LTO, _PIPELINED],
)

def _rust_artifact_impl(ctx):
    crate = ctx.attr.crate[0]
    info = crate[DefaultInfo]
    if not ctx.attr._executable:
        # Each file again at <name>/<basename> in this target's own configuration: a path that names neither the
        # transition's configuration nor the flags (a linker records it in the debug map, which the binary's LC_UUID
        # hashes).
        files = []
        for file in info.files.to_list():
            link = ctx.actions.declare_file(ctx.label.name + "/" + file.basename)
            ctx.actions.symlink(output = link, target_file = file)
            files.append(link)
        return [DefaultInfo(files = depset(files))]

    # A binary: runnable from here (bazel run), with the crate's runfiles.
    executable = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(output = executable, target_file = info.files_to_run.executable, is_executable = True)
    return [DefaultInfo(
        executable = executable,
        files = depset([executable]),
        runfiles = ctx.runfiles([executable]).merge(info.default_runfiles),
    )]

_ATTRS = {
    "crate": attr.label(mandatory = True, cfg = _rust_transition, doc = "a rust_static_library, rust_library or rust_binary"),
    "kind": attr.string(mandatory = True, values = ["device", "device_mac", "tool"]),
}

rust_artifact = rule(
    implementation = _rust_artifact_impl,
    doc = "`crate` built for `kind` (see the module docstring), in the profile //bazel:profile names.",
    attrs = _ATTRS | {"_executable": attr.bool(default = False)},
)

rust_artifact_binary = rule(
    implementation = _rust_artifact_impl,
    doc = "A rust_artifact of a rust_binary: `bazel run` runs it.",
    attrs = _ATTRS | {"_executable": attr.bool(default = True)},
    executable = True,
)

# lint_files() exclusions for a third-party source tree: its own files, not its BUILD files.
THIRD_PARTY_LINT_EXCLUDE = [
    "**/*.c",
    "**/*.h",
    "**/*.json",
    "**/*.md",
    "**/*.py",
    "**/*.rs",
    "**/*.sh",
    "**/*.toml",
]

def cargo_ids(metadata, extra_filename):
    """The -C metadata and -C extra-filename cargo gave a crate (bazel/rust/rules_rust.patch)."""
    return ["-Cmetadata=" + metadata, "-Cextra-filename=-" + extra_filename]
