"""iOS 27's translated parts as Bazel actions (tools/install/translate.py): each role's pretranslate delta, and each
cryptex rebuilt from its delta, given its owners and sealed. A rerun after any failure redoes only what is missing.

They attach disk images, so they run unsandboxed on this Mac (.bazelrc gives their mnemonics the local strategy).
"""

load("//translate/arm64e/pretranslate:defs.bzl", "PretranslatedInfo")

# no-remote keeps the IPSW's translated bytes off any remote cache; the local disk cache keeps them, so a new output
# base or a `bazel clean` takes them from there instead of translating again.
_LOCAL = {"no-remote": "", "no-sandbox": "", "requires-darwin": ""}

DeltaInfo = provider(doc = "A role's delta directory.", fields = {"role": "os, app_cryptex or system_cryptex", "dir": "File"})

def _pretranslated_delta_impl(ctx):
    info = ctx.attr.translator[PretranslatedInfo]
    out = ctx.actions.declare_directory(ctx.label.name)
    args = ctx.actions.args()
    args.add("delta")
    args.add("--role", info.role)
    args.add("--image", ctx.file.image)
    args.add("--pins", ctx.file.pins)
    args.add("--driver", info.driver)
    args.add("--tool", info.tool)
    args.add_all(info.apple_trustcaches, before_each = "--apple-trustcache")
    pairs = [delta[DeltaInfo] for delta in ctx.attr.pairs]
    for pair in pairs:
        args.add("--cdhash-pairs", "{}/{}.cdhash-pairs".format(pair.dir.path, pair.role))
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._translate,
        arguments = [args],
        inputs = [ctx.file.image, ctx.file.pins] + info.apple_trustcaches + [pair.dir for pair in pairs],
        tools = [info.driver_run, info.tool_run],
        outputs = [out],
        mnemonic = "Pretranslate",
        progress_message = "Translating %s's arm64e code to arm64 (%%{label})" % info.role,
        execution_requirements = _LOCAL,
    )
    return [DefaultInfo(files = depset([out])), DeltaInfo(role = info.role, dir = out)]

pretranslated_delta = rule(
    implementation = _pretranslated_delta_impl,
    doc = "One role's delta: the translated files, their manifest, trust module and cdhash pairs (translate_tree).",
    attrs = {
        "image": attr.label(mandatory = True, allow_single_file = True, doc = "the decrypted member"),
        "pins": attr.label(mandatory = True, allow_single_file = True, doc = "the IPSW's pins: the System volume's UUID"),
        "translator": attr.label(mandatory = True, providers = [PretranslatedInfo]),
        "pairs": attr.label_list(providers = [DeltaInfo], doc = "the deltas whose cdhash pairs the dyld cache takes"),
        "_translate": attr.label(default = "//tools/install:translate", executable = True, cfg = "exec"),
    },
)

def _translated_cryptex_impl(ctx):
    image = ctx.actions.declare_file(ctx.label.name + "/" + ctx.file.image.basename)
    sha256 = ctx.actions.declare_file(ctx.label.name + "/" + ctx.file.image.basename + ".sha256")
    delta = ctx.attr.delta[DeltaInfo].dir
    args = ctx.actions.args()
    args.add("cryptex")
    args.add("--image", ctx.file.image)
    args.add("--delta", delta.path)
    args.add("--role", ctx.attr.role)
    args.add("--pins", ctx.file.pins)
    args.add("--apfs-tool", ctx.file.apfs_tool)
    args.add("--out", image)
    args.add("--sha256", sha256)
    ctx.actions.run(
        executable = ctx.executable._translate,
        arguments = [args],
        inputs = [ctx.file.image, ctx.file.pins, delta],
        tools = [ctx.file.apfs_tool],
        outputs = [image, sha256],
        mnemonic = "TranslatedCryptex",
        progress_message = "Rebuilding and sealing the translated %s (%%{label})" % ctx.attr.role,
        execution_requirements = _LOCAL,
    )
    return [DefaultInfo(files = depset([image]), runfiles = ctx.runfiles(files = [sha256]))]

translated_cryptex = rule(
    implementation = _translated_cryptex_impl,
    doc = "A cryptex with its delta in place, Apple's owners, sealed and fitted, under the member's own file name; " +
          "its sha256 beside it (in runfiles).",
    attrs = {
        "image": attr.label(mandatory = True, allow_single_file = True, doc = "the decrypted member"),
        "delta": attr.label(mandatory = True, providers = [DeltaInfo], doc = "its pretranslated_delta"),
        "role": attr.string(mandatory = True, values = ["app_cryptex", "system_cryptex"]),
        "pins": attr.label(mandatory = True, allow_single_file = True, doc = "the IPSW's pins: the cryptex's UUID"),
        "apfs_tool": attr.label(mandatory = True, cfg = "exec", allow_single_file = True),
        "_translate": attr.label(default = "//tools/install:translate", executable = True, cfg = "exec"),
    },
)
