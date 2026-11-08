"""Rules behind //install: the kernel-derived install spec, the IPSW pins as JSON, the install artifacts."""

load("//ipsw:defs.bzl", _ipsw_pins = "ipsw_pins")
load("//kernel:prelink.bzl", "PRELINK")
load("//kernel/rules:kernel.bzl", "KernelObjectsInfo")

# The selected IPSW kind's member pins as JSON (//ipsw:defs.bzl).
ipsw_pins = _ipsw_pins

def _install_spec_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".json")
    sources = {
        "raw-mount": ctx.file.raw_mount,
        "cryptex-h": ctx.file.cryptex_h,
        "cryptex-cpp": ctx.file.cryptex_cpp,
        "block-cpp": ctx.file.block_cpp,
        "volumes-h": ctx.file.volumes_h,
    }
    args = ctx.actions.args()
    for flag, f in sources.items():
        args.add("--" + flag, f)
    args.add("--out", out)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = sources.values(),
        outputs = [out],
        mnemonic = "InstallSpec",
        progress_message = "Reading the SD layout the kernel expects from its sources",
    )
    return [DefaultInfo(files = depset([out]))]

install_spec = rule(
    implementation = _install_spec_impl,
    attrs = {
        "raw_mount": attr.label(allow_single_file = True, mandatory = True),
        "cryptex_h": attr.label(allow_single_file = True, mandatory = True),
        "cryptex_cpp": attr.label(allow_single_file = True, mandatory = True),
        "block_cpp": attr.label(allow_single_file = True, mandatory = True),
        "volumes_h": attr.label(allow_single_file = True, mandatory = True),
        "_tool": attr.label(default = "//tools/install:spec", executable = True, cfg = "exec"),
    },
    doc = "The partition size and volume/cryptex UUIDs the kernel was built with, parsed from its sources.",
)

def _install_artifacts_impl(ctx):
    out = ctx.actions.declare_directory(ctx.label.name)
    package = ctx.file.boot_package
    args = ctx.actions.args()
    args.add("--spec", ctx.file.spec)
    args.add("--ipsw", ctx.file.ipsw)
    args.add("--boot-package", package.path)
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [ctx.file.spec, ctx.file.ipsw, package],
        outputs = [out],
        mnemonic = "InstallArtifacts",
        progress_message = "Describing the SD install",
    )
    return [DefaultInfo(files = depset([out]))]

install_artifacts = rule(
    implementation = _install_artifacts_impl,
    attrs = {
        "spec": attr.label(allow_single_file = True, mandatory = True),
        "ipsw": attr.label(allow_single_file = True, mandatory = True),
        "boot_package": attr.label(allow_single_file = True, mandatory = True),
        "_tool": attr.label(default = "//tools/install:artifacts", executable = True, cfg = "exec"),
    },
)

def _test_member_impl(ctx):
    member = ctx.actions.declare_file(ctx.label.name + ".bin")
    pins = ctx.actions.declare_file(ctx.label.name + ".pins.json")
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [ctx.file.pins.path, ctx.attr.role, str(ctx.attr.mib), member.path, pins.path],
        inputs = [ctx.file.pins],
        outputs = [member, pins],
        mnemonic = "TestMember",
    )
    return [
        DefaultInfo(files = depset([member])),
        OutputGroupInfo(pins = depset([pins])),
    ]

test_member = rule(
    implementation = _test_member_impl,
    attrs = {
        "pins": attr.label(allow_single_file = True, mandatory = True),
        "role": attr.string(mandatory = True),
        "mib": attr.int(default = 4),
        "_tool": attr.label(default = "//tools/install:test_member", executable = True, cfg = "exec"),
    },
    doc = "A small stand-in for a large IPSW member plus pins that match it (low-space installer tests).",
)

def _fake_boot_package_impl(ctx):
    out = ctx.actions.declare_directory(ctx.label.name)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [out.path, "--generation", str(ctx.attr.generation)] + (["--gpu"] if ctx.attr.gpu else []),
        outputs = [out],
        mnemonic = "FakeBootPackage",
    )
    return [DefaultInfo(files = depset([out]))]

fake_boot_package = rule(
    implementation = _fake_boot_package_impl,
    attrs = {
        "generation": attr.int(default = 0),
        "gpu": attr.bool(default = False, doc = "make a fake //gpu:bundle instead"),
        "_tool": attr.label(default = "//tools/install:fake_boot_package", executable = True, cfg = "exec"),
    },
    doc = "A boot package with dummy contents and the real layout, for tests (--//deploy:boot_package=...).",
)

def _prelink_options_impl(ctx):
    kernel = ctx.attr.kernel[KernelObjectsInfo]
    macho = ctx.actions.declare_file(ctx.label.name + "/kernel.macho")
    options = ctx.actions.declare_file(ctx.label.name + "/prelink.json")
    ctx.actions.symlink(output = macho, target_file = kernel.objects[0])
    ctx.actions.write(options, json.encode_indent({
        "cuts": kernel.cuts,
        "defers": kernel.deferred,
        "new_roots": sorted({r: True for r in kernel.prelink}.keys()),
    } | PRELINK) + "\n")
    return [DefaultInfo(files = depset([macho, options]))]

prelink_options = rule(
    implementation = _prelink_options_impl,
    attrs = {"kernel": attr.label(mandatory = True, providers = [KernelObjectsInfo])},
    doc = "The linked kernel before the prelink, and what the prelink takes besides it (prelink.json).",
)

def _release_prelink_impl(ctx):
    plan = ctx.actions.declare_file(ctx.label.name + "/kernel.prelink-plan")
    out = ctx.actions.declare_file(ctx.label.name + "/kernel.macho")
    args = ctx.actions.args()
    args.add("plan")
    args.add("--prelinked", ctx.file.kernelcache)
    args.add("--inherit-original-report", ctx.file.inherit_report)
    args.add("--previous", ctx.file.previous)
    args.add("--options", ctx.file.options)
    args.add("--plan-output", plan)
    ctx.actions.run(
        executable = ctx.executable._emit_prelink,
        arguments = [args],
        inputs = [ctx.file.kernelcache, ctx.file.inherit_report, ctx.file.previous, ctx.file.options],
        outputs = [plan],
        mnemonic = "KernelPrelinkPlan",
        progress_message = "Planning the prelink of the original kexts",
    )
    ctx.actions.run(
        executable = ctx.executable._emit_prelink,
        arguments = ["apply", "--plan", plan.path, "--kernel", ctx.file.kernel.path, "--output", out.path],
        inputs = [plan, ctx.file.kernel],
        outputs = [out],
        mnemonic = "KernelPrelink",
        progress_message = "Prelinking the original kexts into the release's kernel",
    )
    return [DefaultInfo(files = depset([out]))]

release_prelink = rule(
    implementation = _release_prelink_impl,
    doc = "prelink_options' kernel with Apple's original kexts prelinked: kernel_prelink's two steps, its options " +
          "from prelink.json. Python only, so a Mac without Xcode can run it.",
    attrs = {
        "kernel": attr.label(mandatory = True, allow_single_file = True, doc = "the linked kernel (kernel.macho)"),
        "options": attr.label(mandatory = True, allow_single_file = True, doc = "its prelink.json"),
        "kernelcache": attr.label(default = "//ipsw:kernelcache", allow_single_file = True),
        "inherit_report": attr.label(mandatory = True, allow_single_file = True),
        "previous": attr.label(mandatory = True, allow_single_file = True),
        "_emit_prelink": attr.label(default = "//tools/prelink:emit_prelink", executable = True, cfg = "exec"),
    },
)

def _release_tree_impl(ctx):
    out = ctx.actions.declare_directory(ctx.label.name)
    commands, inputs = [], []
    for target, destination in ctx.attr.files.items():
        for f in target.files.to_list():
            inputs.append(f)
            where = destination if len(target.files.to_list()) == 1 else destination + "/" + f.basename
            commands.append('/bin/mkdir -p "$1/{parent}" && /bin/cp -RL "{src}" "$1/{dest}"'.format(
                parent = where.rpartition("/")[0],
                src = f.path,
                dest = where,
            ))
    ctx.actions.run_shell(
        command = "set -e\n" + "\n".join(commands),
        arguments = [out.path],
        inputs = inputs,
        outputs = [out],
        mnemonic = "ReleaseTree",
        progress_message = "Laying out %{label}",
    )
    return [DefaultInfo(files = depset([out]))]

release_tree = rule(
    implementation = _release_tree_impl,
    attrs = {"files": attr.label_keyed_string_dict(allow_files = True, doc = "each target's file(s) -> its path")},
    doc = "A directory with each target at the path //:image reads it from (one release asset).",
)

def _rlocation(ctx, f):
    return f.short_path[3:] if f.short_path.startswith("../") else ctx.workspace_name + "/" + f.short_path

def _image_inputs_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".json")
    paths, files = {}, []
    for name, target in ctx.attr.inputs.items():
        found = target[DefaultInfo].files.to_list()
        if len(found) > 1:
            fail("{} ({}) must be one file or directory, not {}".format(name, target.label, len(found)))
        if found:
            paths[name] = _rlocation(ctx, found[0])
            files.append(found[0])
    runfiles = ctx.runfiles(files = [out] + files)
    for target in ctx.attr.inputs.values():
        runfiles = runfiles.merge(target[DefaultInfo].default_runfiles)
    ctx.actions.write(out, json.encode_indent(paths) + "\n")
    return [DefaultInfo(files = depset([out]), runfiles = runfiles)]

image_inputs = rule(
    implementation = _image_inputs_impl,
    attrs = {
        "inputs": attr.string_keyed_label_dict(allow_files = True),
    },
    doc = "{input name: runfiles path} as JSON for tools/install/image.py, with the inputs and their runfiles in " +
          "runfiles; an empty input (a label_flag left at :none) is left out.",
)
