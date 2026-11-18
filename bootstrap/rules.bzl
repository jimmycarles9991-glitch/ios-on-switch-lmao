"""Build the adapted package manager and hook engine, then assemble one Data seed."""

def _root(files, suffix):
    candidates = [file for file in files if file.path.endswith(suffix)]
    if not candidates:
        fail("missing source marker ending in " + suffix)
    marker = sorted([(len(file.path), file.path) for file in candidates])[0][1]
    return marker[:-len(suffix)].rstrip("/")

def _source_impl(ctx):
    out = ctx.actions.declare_directory(ctx.label.name)
    args = ctx.actions.args()
    args.add("--source", _root(ctx.files.srcs, "/" + ctx.attr.project + ".xcodeproj/project.pbxproj"))
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--out", out.path)
    inputs = ctx.files.srcs + [ctx.file._xcode_env, ctx.file._xcode_identity]
    for target, name in ctx.attr.dependencies.items():
        files = target.files.to_list()
        args.add("--dependency", name + "=" + _root(files, "/Package.swift"))
        inputs += files
    ctx.actions.run(
        executable = ctx.executable.tool,
        arguments = [args],
        inputs = inputs,
        outputs = [out],
        # SwiftPM runs manifests in its own macOS sandbox.
        execution_requirements = {"requires-darwin": "", "no-sandbox": ""},
        mnemonic = "BootstrapSources",
        progress_message = "Building " + ctx.attr.project + " for the Switch rootless bootstrap",
    )
    return [DefaultInfo(files = depset([out]))]

xcode_source_product = rule(
    implementation = _source_impl,
    attrs = {
        "srcs": attr.label(allow_files = True, mandatory = True),
        "project": attr.string(mandatory = True),
        "dependencies": attr.label_keyed_string_dict(),
        "tool": attr.label(executable = True, cfg = "exec", mandatory = True),
        "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
        "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
    },
)

def _payload_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".tar")
    manifest = ctx.actions.declare_file(ctx.label.name + ".manifest.json")
    packages = ctx.actions.declare_directory(ctx.label.name + ".packages")
    args = ctx.actions.args()
    files = {
        "base": ctx.file.base,
        "sileo-deb": ctx.file.sileo_deb,
        "sileo-built": ctx.file.sileo_built,
        "built": ctx.file.built,
        "uicache": ctx.file.uicache,
        "sbreload": ctx.file.sbreload,
        "launchctl": ctx.file.launchctl,
        "launchctl-license": ctx.file._launchctl_license,
        "sources": ctx.file.sources,
        "zstd-lib": ctx.file._zstd,
    }
    for name, file in files.items():
        args.add("--" + name, file.path)
    args.add("--out", out)
    args.add("--manifest", manifest)
    args.add("--debs", packages.path)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = files.values(),
        outputs = [out, manifest, packages],
        execution_requirements = {"requires-darwin": ""},
        mnemonic = "BootstrapPayload",
        progress_message = "Assembling and signing the rootless Sileo bootstrap",
    )
    managed = {}
    for name in ("sileo", "ellekit", "uikittools", "launchctl"):
        package = ctx.actions.declare_file(ctx.label.name + "." + name + ".deb")
        ctx.actions.run_shell(
            inputs = [packages],
            outputs = [package],
            arguments = [packages.path + "/" + name + ".deb", package.path],
            command = '/bin/cp "$1" "$2"',
            mnemonic = "BootstrapManagedPackage",
        )
        managed["managed_" + name] = depset([package])
    return [
        DefaultInfo(files = depset([out])),
        OutputGroupInfo(manifest = depset([manifest]), packages = depset([packages]), **managed),
    ]

bootstrap_payload = rule(
    implementation = _payload_impl,
    attrs = {
        "base": attr.label(allow_single_file = True, mandatory = True),
        "sileo_built": attr.label(allow_single_file = True, mandatory = True),
        "sileo_deb": attr.label(allow_single_file = True, mandatory = True),
        "built": attr.label(allow_single_file = True, mandatory = True),
        "uicache": attr.label(default = "//drivers/SwitchAppRegistration:switch-appregd", allow_single_file = True),
        "sbreload": attr.label(default = "//bootstrap/respring:sbreload", allow_single_file = True),
        "launchctl": attr.label(default = "//bootstrap/launchctl:launchctl", allow_single_file = True),
        "_launchctl_license": attr.label(default = "@bootstrap_launchctl//:LICENSE", allow_single_file = True),
        "sources": attr.label(default = "//bootstrap:sources.json", allow_single_file = True),
        "_zstd": attr.label(default = "//bootstrap:zstd", allow_single_file = True, cfg = "exec"),
        "_tool": attr.label(default = "//bootstrap:assemble", executable = True, cfg = "exec"),
    },
)
