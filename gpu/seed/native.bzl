"""The native seed's actions (metal_seed's, defs.bzl): the driver's records, made per corpus bucket by the offline
compile, stamped for the tools' stack into one per-file tree that the host checks read, then packed as the device reads
them (kits/SwitchSeedPack): <packed>/raw-<stack>, an index and ~12 MiB packs, the observed states' packs first.
"""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//ipsw:ipsw.bzl", "IpswInfo")
load(":tools.bzl", "raw_tool_files")

DARWIN = {"requires-darwin": ""}

# What tools/gpu/shaders/native_seed.py runs of the raw tools (bucket and stamp): none of the others
_NATIVE_TOOLS = ["native-seed", "libmock_iokit.dylib"]

def named(ctx, path):
    return "{}/{}{}".format(ctx.attr.ipsw[IpswInfo].kind, ctx.attr.name, path)

def three(i):
    return ("00" + str(i))[-3:]

def bucket_resources(_os, _inputs):
    return {"cpu": 1, "memory": 1024}

def native_records(ctx, tools, crawl, views, lines):
    """Make, stamp and check the native records; returns the stamped tree and the coverage check.

    Bucket i reads views[i] of the libraries (its root, files and store; metal_seed's plan)."""
    native_tools = raw_tool_files(tools, _NATIVE_TOOLS)
    buckets = []
    for i, corpus in enumerate(lines):
        view = views[i]
        inputs = depset(
            native_tools + [view.store] + ctx.files.device_libraries + ctx.files.embedded,
            transitive = [view.files],
        )
        out = ctx.actions.declare_directory(named(ctx, "_native/bucket-" + three(i)))
        args = ctx.actions.args()
        args.add("bucket")
        args.add("--raw", tools.raw_dir)
        args.add("--root", view.root + "/root")
        args.add("--store", view.store)
        args.add("--qc", crawl.qc)
        args.add("--lines", corpus)
        args.add_all(ctx.files.embedded, before_each = "--embedded")
        args.add("--out", out.path)
        ctx.actions.run(
            executable = ctx.executable._native_seed,
            arguments = [args],
            inputs = depset([corpus], transitive = [inputs]),
            outputs = [out],
            mnemonic = "MetalNativeBucket",
            progress_message = "Compiling native shader records {} of {}".format(i + 1, len(lines)),
            execution_requirements = DARWIN,
            resource_set = bucket_resources,
        )
        buckets.append(out)
    native_records = ctx.actions.declare_directory(named(ctx, "_native/stamped"))
    args = ctx.actions.args()
    args.add("stamp")
    args.add("--raw", tools.raw_dir)
    args.add("--manifest", tools.manifest)
    args.add("--ipsw", ctx.attr.ipsw[IpswInfo].build)
    args.add_all(lines, before_each = "--lines")
    args.add_all(buckets, before_each = "--bucket", expand_directories = False)
    args.add("--out", native_records.path)
    ctx.actions.run(
        executable = ctx.executable._native_seed,
        arguments = [args],
        inputs = buckets + lines + native_tools + [tools.manifest],
        outputs = [native_records],
        mnemonic = "MetalNativeStamp",
        progress_message = "Stamping native records for this OS and compiler",
        execution_requirements = DARWIN,
    )
    checked = ctx.actions.declare_file(named(ctx, "_native/coverage.json"))
    args = ctx.actions.args()
    args.add("check")
    args.add("--seed", native_records.path)
    args.add("--out", checked)
    if ctx.attr._profile[BuildSettingInfo].value == "release":
        args.add("--strict")
    ctx.actions.run(
        executable = ctx.executable._native_seed,
        arguments = [args],
        inputs = [native_records],
        outputs = [checked],
        mnemonic = "MetalNativeCoverage",
        progress_message = "Checking native render seed coverage",
    )
    return native_records, checked

def native_pack(ctx, stamped):
    """Pack the stamped tree for the device."""
    packed = ctx.actions.declare_directory(named(ctx, "_native/packed"))
    args = ctx.actions.args()
    args.add("pack")
    args.add("--seed", stamped.path)
    args.add("--tool", ctx.executable._seedpack)
    args.add("--out", packed.path)
    ctx.actions.run(
        executable = ctx.executable._native_seed,
        arguments = [args],
        inputs = [stamped],
        tools = [ctx.executable._seedpack],
        outputs = [packed],
        mnemonic = "MetalNativePack",
        progress_message = "Packing native records for the device",
    )
    return packed
