"""The one host input the build declares: Xcode, its toolchain and its iOS/macOS SDKs.

Apple's licence does not allow downloading or redistributing them, so the build cannot fetch them. Instead this
repository rule finds the Xcode named by $DEVELOPER_DIR (or `xcode-select -p`), checks it against the pins below,
and writes @xcode//:identity, a file every action that runs an Xcode tool takes as an input. Changing Xcode
therefore changes those actions' keys (and nothing else's), and an unpinned Xcode is refused before anything runs.

Bump: install the new Xcode, `xcode-select -s` it (or export DEVELOPER_DIR), run `bazel run @xcode//:print_pins`
and paste its output over XCODE_PINS, then re-record what XNU's make does with it (`bazel run
//kernel/xnu-<version>:update_tables` for each XNU, `bazel run //bazel/toolchains/kernel:update_recipes`). The
kernel's code generation depends on Apple clang's version, so expect every kernel object to rebuild.
"""

# CI selects the Xcode with this build: GitHub's xcode-27 runner image must keep it (/Applications/Xcode_27.2.app).
XCODE_PINS = {
    "version": "27.2",
    "build": "27B52",
    "clang": "Apple clang version 21.0.0 (clang-2100.4.1.1)",
    "ld": "27106.1",
    "sha256": {
        "Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk/SDKSettings.json": "8c5d2efa53dbdb9b602c45e77c5eac5b21165317408fe37ddf14c7aa9c8b9031",
        "Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/SDKSettings.json": "82165b15ee21419d4cffaba8895797391b908ac9499ba70f561f3e1bd16758d6",
    },
    # The toolchain by its Mach-O UUIDs: the App Store's copy of an Xcode and the .xip that GitHub's runner images
    # install sign the same binaries differently, so their bytes differ and their code does not.
    "uuid": {
        "Toolchains/XcodeDefault.xctoolchain/usr/bin/clang": "24E6E74C-3725-35DC-98F2-E2D5A986D10C",
        "Toolchains/XcodeDefault.xctoolchain/usr/bin/ld": "ACFD815E-3031-3E2F-98DF-6759C331775D",
    },
    # Xcode's separately installed Metal toolchain (Settings > Components, ~/Library/Developer/DVTDownloads): its
    # frontend version is in every function of a metallib it makes, and no action key holds it. Checked, not part of
    # identity: a Mac with another version is refused instead of filling the caches with other bytes under the same
    # keys. A Mac without it builds what needs no Metal compiler.
    "metal": "Apple metal version 32023.922 (metalfe-32023.922.4)",
}

_BUILD = """\
package(default_visibility = ["//visibility:public"])

exports_files(["identity", "xcode.env"])

# `bazel run @xcode//:print_pins`: the XCODE_PINS for the selected Xcode (to bump the pin).
sh_binary(
    name = "print_pins",
    srcs = ["print_pins.sh"],
)
"""

_NO_XCODE_BUILD = """\
load(":missing.bzl", "missing")

package(default_visibility = ["//visibility:public"])

missing(name = "identity")

missing(name = "xcode.env")

missing(name = "print_pins")
"""

_NO_XCODE_RULE = """\
def _impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".missing")
    ctx.actions.run_shell(
        outputs = [out],
        command = "printf '%s\\\\n' \\"$1\\" >&2; exit 1",
        arguments = [{message}],
        mnemonic = "NoXcode",
    )
    return [DefaultInfo(files = depset([out]), executable = out)]

missing = rule(implementation = _impl, executable = True)
"""

# Without an Xcode the build can still load and analyse: the registered toolchains and the kernel's rules load
# @xcode//:defs.bzl, so even `bazel run //:image_dry_run --//install:prebuilt=release`, which runs no Xcode tool,
# fetches this repository. Only an action that takes @xcode//:identity or :xcode.env fails, and says why.
_NO_XCODE = "/var/empty/no-xcode/Contents/Developer"

def _no_xcode(rctx, reason):
    message = "No Xcode: {}. This runs Xcode's tools: install Xcode {} ({}) and `sudo xcode-select -s` it.".format(
        reason,
        XCODE_PINS["version"],
        XCODE_PINS["build"],
    )
    toolchain = _NO_XCODE + "/Toolchains/XcodeDefault.xctoolchain/usr/bin"
    rctx.file("missing.bzl", _NO_XCODE_RULE.format(message = repr(message)))
    rctx.file("defs.bzl", "\n".join([
        "DEVELOPER_DIR = {}".format(repr(_NO_XCODE)),
        "XCODE_TOOLCHAIN = {}".format(repr(toolchain)),
        "IPHONEOS_SDK = {}".format(repr(_NO_XCODE + "/SDKs/iPhoneOS.sdk")),
        "MACOSX_SDK = {}".format(repr(_NO_XCODE + "/SDKs/MacOSX.sdk")),
        "XCODE_BUILD = ''",
    ]) + "\n")
    rctx.file("BUILD.bazel", _NO_XCODE_BUILD)

def _run(rctx, args, env = {}):
    result = rctx.execute(args, environment = env, quiet = True)
    if result.return_code:
        fail("{} failed ({}): {}".format(" ".join(args), result.return_code, result.stderr))
    return result.stdout.strip()

def _developer_dir(rctx):
    """The selected developer directory, or None and why there is none."""
    developer = rctx.getenv("DEVELOPER_DIR")
    if not developer:
        result = rctx.execute(["/usr/bin/xcode-select", "-p"], quiet = True)
        if result.return_code:
            return None, "`xcode-select -p` failed: " + (result.stderr.strip() or "exit {}".format(result.return_code))
        developer = result.stdout.strip()
    if not developer.endswith("/Contents/Developer"):
        return None, "{} is not an Xcode (Command Line Tools have no iOS SDK)".format(developer)
    return developer, None

def _macho_uuid(rctx, toolchain, path):
    for line in _run(rctx, [toolchain + "/dwarfdump", "--uuid", "--arch", "arm64", path]).splitlines():
        if line.startswith("UUID: "):
            return line.split(" ")[1]
    fail("{} has no arm64 UUID".format(path))

def _metal_version(rctx, developer):
    """The selected Metal toolchain's `metal --version` line, or None where no Metal toolchain is installed."""
    env = {"DEVELOPER_DIR": developer}
    found = rctx.execute(["/usr/bin/xcrun", "--sdk", "iphoneos", "--find", "metal"], environment = env, quiet = True)
    if found.return_code:
        return None
    rctx.watch(found.stdout.strip())
    version = rctx.execute(
        ["/usr/bin/xcrun", "--sdk", "iphoneos", "metal", "--version"],
        environment = env,
        quiet = True,
    )
    return version.stdout.splitlines()[0] if not version.return_code and version.stdout else None

def _xcode_host_impl(rctx):
    developer, reason = _developer_dir(rctx)
    if not developer:
        _no_xcode(rctx, reason)
        return
    app = developer[:-len("/Contents/Developer")]
    plist = app + "/Contents/version.plist"
    rctx.watch(plist)
    if not rctx.path(plist).exists:
        _no_xcode(rctx, "{} has no {}".format(developer, plist))
        return
    version = _run(rctx, ["/usr/libexec/PlistBuddy", "-c", "Print :CFBundleShortVersionString", plist])
    build = _run(rctx, ["/usr/libexec/PlistBuddy", "-c", "Print :ProductBuildVersion", plist])
    toolchain = developer + "/Toolchains/XcodeDefault.xctoolchain/usr/bin"
    clang = _run(rctx, [toolchain + "/clang", "--version"]).splitlines()[0]
    ld = ""
    for line in _run(rctx, [toolchain + "/ld", "-version_details"]).splitlines():
        if '"version"' in line:
            ld = line.split(":")[1].strip().strip(",").strip('"')
            break
    digests = {}
    for relative in sorted(list(XCODE_PINS["sha256"]) + list(XCODE_PINS["uuid"])):
        path = developer + "/" + relative
        rctx.watch(path)
        digests[relative] = _run(rctx, ["/usr/bin/shasum", "-a", "256", path]).split(" ")[0]

    pinned_digests = {k: digests[k] for k in XCODE_PINS["sha256"]}
    uuids = {k: _macho_uuid(rctx, toolchain, developer + "/" + k) for k in sorted(XCODE_PINS["uuid"])}
    found = {"version": version, "build": build, "clang": clang, "ld": ld, "sha256": pinned_digests, "uuid": uuids}
    found["metal"] = _metal_version(rctx, developer) or XCODE_PINS["metal"]
    mismatches = [k for k in found if found[k] != XCODE_PINS[k]]
    pins = json.encode_indent(found, indent = "    ")
    if mismatches and rctx.getenv("SWITCH_ALLOW_UNPINNED_XCODE") != "1":
        fail(("\n\nThe selected Xcode ({}) does not match the pin in bazel/host/xcode.bzl ({} differ).\n" +
              "Select the pinned Xcode {} ({}) with `sudo xcode-select -s <Xcode.app>` or export DEVELOPER_DIR,\n" +
              "or bump the pin to this Xcode (every kernel object rebuilds):\n\nXCODE_PINS = {}\n").format(
            app,
            ", ".join(mismatches),
            XCODE_PINS["version"],
            XCODE_PINS["build"],
            pins,
        ))
    if mismatches:
        # buildifier: disable=print
        print("WARNING: building with an UNPINNED Xcode ({}); outputs will not match the pinned build.".format(app))

    # The SDK paths as `xcrun --show-sdk-path` names them (iPhoneOS27.2.sdk): the spelling the CMake flow passed to
    # -isysroot, which ends up in debug info.
    xcrun_env = {"DEVELOPER_DIR": developer}
    iphoneos = _run(rctx, ["/usr/bin/xcrun", "--sdk", "iphoneos", "--show-sdk-path"], xcrun_env)
    macosx = _run(rctx, ["/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-path"], xcrun_env)
    rctx.file("identity", "\n".join([
        "developer_dir " + developer,
        "version " + version,
        "build " + build,
        "clang " + clang,
        "ld " + ld,
    ] + ["sha256 {} {}".format(digests[k], k) for k in sorted(digests)]) + "\n")

    # Sourced by the build's action wrappers (bazel/tools/xcode_env.sh): the absolute host paths, never PATH lookups.
    rctx.file("xcode.env", "\n".join([
        "DEVELOPER_DIR=" + developer,
        "XCODE_TOOLCHAIN=" + toolchain,
        "IPHONEOS_SDK=" + iphoneos,
        "MACOSX_SDK=" + macosx,
    ]) + "\n")
    rctx.file("print_pins.sh", "#!/bin/sh\ncat <<'EOF'\nXCODE_PINS = {}\nEOF\n".format(pins), executable = True)
    rctx.file("defs.bzl", "\n".join([
        "DEVELOPER_DIR = {}".format(repr(developer)),
        "XCODE_TOOLCHAIN = {}".format(repr(toolchain)),
        "IPHONEOS_SDK = {}".format(repr(iphoneos)),
        "MACOSX_SDK = {}".format(repr(macosx)),
        "XCODE_BUILD = {}".format(repr(build)),
    ]) + "\n")
    rctx.file("BUILD.bazel", 'load("@rules_shell//shell:sh_binary.bzl", "sh_binary")\n\n' + _BUILD)

xcode_host = repository_rule(
    implementation = _xcode_host_impl,
    environ = ["DEVELOPER_DIR", "SWITCH_ALLOW_UNPINNED_XCODE"],
    local = True,
    doc = "Finds and checks the host Xcode (the build's only undownloadable input).",
)
