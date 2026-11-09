"""What the build derives from an IPSW's members. Each rule takes the IPSW (//ipsw:current, the kind
--//ipsw:kind selects) for the kind's derived pins and names, and its members from @ipsw (the same kind's)."""

load("//ipsw:ipsw.bzl", "IpswInfo")
load("//ipsw:pins.bzl", "IPSWS")

_IPSW = {"ipsw": attr.label(default = "//ipsw:current", providers = [IpswInfo])}

def _ipsw_kernelcache_impl(ctx):
    ipsw = ctx.attr.ipsw[IpswInfo]
    derived, build = ipsw.derived, ipsw.build
    if ctx.attr.kind:
        derived, build = IPSWS[ctx.attr.kind]["derived"], ctx.attr.kind
    out = ctx.actions.declare_file(ctx.attr.out)
    ctx.actions.run(
        executable = ctx.executable._im4p,
        arguments = [
            ctx.file.src.path,
            out.path,
            "--expect-type=krnl",
            "--lzfse",
            "--thin-arm64",
            "--sha256=" + derived["kernelcache_sha256"],
        ],
        inputs = [ctx.file.src],
        outputs = [out],
        mnemonic = "IpswKernelcache",
        progress_message = "Decompressing the %s kernelcache" % build,
        # LZFSE through macOS's compression_tool.
        execution_requirements = {"requires-darwin": ""},
    )
    return [DefaultInfo(files = depset([out]))]

ipsw_kernelcache = rule(
    implementation = _ipsw_kernelcache_impl,
    doc = "The kernelcache's IM4P 'krnl' payload, LZFSE-decompressed, its arm64 slice; checked against the kind's pin.",
    attrs = _IPSW | {
        "kind": attr.string(doc = "a fixed IPSW kind whose pin src is checked against (default: the selected kind's)"),
        "src": attr.label(mandatory = True, allow_single_file = True),
        "out": attr.string(mandatory = True),
        "_im4p": attr.label(default = "//tools/kernel:im4p", executable = True, cfg = "exec"),
    },
)

def _ipsw_device_tree_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.out)
    ctx.actions.run(
        executable = ctx.executable._im4p,
        arguments = [ctx.file.src.path, out.path, "--expect-type=dtre", "--lzfse"],
        inputs = [ctx.file.src],
        outputs = [out],
        mnemonic = "IpswDeviceTree",
        progress_message = "Decompressing the %s device tree" % ctx.attr.ipsw[IpswInfo].build,
        # LZFSE through macOS's compression_tool.
        execution_requirements = {"requires-darwin": ""},
    )
    return [DefaultInfo(files = depset([out]))]

ipsw_device_tree = rule(
    implementation = _ipsw_device_tree_impl,
    doc = "The device tree IM4P's 'dtre' payload, LZFSE-decompressed: Apple's tree of the IPSW's own device.",
    attrs = _IPSW | {
        "src": attr.label(mandatory = True, allow_single_file = True),
        "out": attr.string(mandatory = True),
        "_im4p": attr.label(default = "//tools/kernel:im4p", executable = True, cfg = "exec"),
    },
)

def _ipsw_apple_trust_impl(ctx):
    ipsw = ctx.attr.ipsw[IpswInfo]
    pin = ipsw.pins["members"][ctx.attr.role]
    if "manifest_digest" not in pin:
        fail("{} has no manifest_digest pinned for {} (ipsw/pin.py)".format(ipsw.build, ctx.attr.role))
    out = ctx.actions.declare_file(ctx.attr.out)
    ctx.actions.run(
        executable = ctx.executable._im4p,
        arguments = [
            ctx.file.src.path,
            out.path,
            "--expect-type=" + ctx.attr.fourcc,
            "--manifest-digest=" + pin["manifest_digest"],
        ],
        inputs = [ctx.file.src],
        outputs = [out],
        mnemonic = "IpswAppleTrust",
        progress_message = "Checking the %s %s against its BuildManifest" % (ipsw.build, pin["component"]),
    )
    return [DefaultInfo(files = depset([out]))]

ipsw_apple_trust = rule(
    implementation = _ipsw_apple_trust_impl,
    doc = "An Apple trust cache's payload, a version 2 module, once its IM4P's SHA-384 is the BuildManifest's Digest " +
          "of it (pins.bzl manifest_digest). The boot package merges it into its static module (tools/package/trust.py).",
    attrs = _IPSW | {
        "role": attr.string(mandatory = True, doc = "the member's role in pins.bzl"),
        "fourcc": attr.string(mandatory = True, doc = "the IM4P type: trst, trcs or trca"),
        "src": attr.label(mandatory = True, allow_single_file = True),
        "out": attr.string(mandatory = True),
        "_im4p": attr.label(default = "//tools/kernel:im4p", executable = True, cfg = "exec"),
    },
)

def _ipsw_pins_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(out, json.encode_indent(ctx.attr.ipsw[IpswInfo].pins, indent = "  ") + "\n")
    return [DefaultInfo(files = depset([out]))]

ipsw_pins = rule(
    implementation = _ipsw_pins_impl,
    doc = "The kind's member pins as JSON (the SD installer checks Preboot copies against their content sha256s).",
    attrs = _IPSW,
)

PretranslatedPinsInfo = provider(
    doc = "iOS 27's pretranslate outputs the boot package carries, each checked against the kind's derived pins.",
    fields = {
        "trust": "list of File: the trust modules of the translated binaries, os, app_cryptex, system_cryptex",
        "cache_list": "File or None: the translated dyld cache files' cdhashes (/chosen/a64x-cache-pretranslated)",
        "pins": "File or None: {file name: sha256}, what the image composer's run must reproduce",
    },
)

_PRETRANSLATE_ROLES = ("os", "app_cryptex", "system_cryptex")
_CACHE_LIST = "a64x-cache-pretranslated.bin"

def _ipsw_pretranslated_impl(ctx):
    pins = ctx.attr.ipsw[IpswInfo].derived.get("pretranslated", {})
    given = {f.basename: f for f in ctx.files.srcs}
    if sorted(given) != sorted(pins):
        fail("{}: the files {} are not the ones ipsw/pins.bzl's derived \"pretranslated\" pins ({})".format(
            ctx.label,
            sorted(given),
            sorted(pins),
        ))
    checked = {}
    for name, src in given.items():
        out = ctx.actions.declare_file(ctx.label.name + "/" + name)
        ctx.actions.run(
            executable = ctx.executable._derived,
            arguments = [src.path, out.path, pins[name]],
            inputs = [src],
            outputs = [out],
            mnemonic = "IpswDerived",
            progress_message = "Checking %s against its pin" % name,
        )
        checked[name] = out
    pins_file = None
    if pins:
        pins_file = ctx.actions.declare_file(ctx.label.name + "/pins.json")
        ctx.actions.write(pins_file, json.encode_indent(pins, indent = "  ") + "\n")
    trust = [checked[role + ".trustcache"] for role in _PRETRANSLATE_ROLES if role + ".trustcache" in checked]
    return [
        DefaultInfo(files = depset(checked.values())),
        PretranslatedPinsInfo(trust = trust, cache_list = checked.get(_CACHE_LIST), pins = pins_file),
        OutputGroupInfo(pins = depset([pins_file] if pins_file else [])),
    ]

ipsw_pretranslated = rule(
    implementation = _ipsw_pretranslated_impl,
    doc = "The kind's pretranslate outputs (//translate/arm64e/pretranslate, run offline on the decrypted images), " +
          "each copied after a sha256 check against pins.bzl's derived \"pretranslated\"; empty for a kind without.",
    attrs = _IPSW | {
        "srcs": attr.label_list(allow_files = True),
        "_derived": attr.label(default = "//ipsw:derived", executable = True, cfg = "exec"),
    },
)
