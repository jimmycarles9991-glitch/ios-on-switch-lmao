"""iOS 27's translated parts as Bazel actions (tools/install/translate.py): each role's pretranslate delta, and each
cryptex rebuilt from its delta, given its owners and sealed. A rerun after any failure redoes only what is missing.

They attach disk images, so they run unsandboxed: on a build host, or on this Mac under --config=noremote (.bazelrc
gives their mnemonics the remote and then the local strategy).
"""

load("//translate/arm64e/pretranslate:defs.bzl", "PretranslatedInfo")

# The IPSW's translated bytes are built and cached on the build hosts: their cache is the owner's own, private to him
# (his decision, 14 Nov 2026), so another output base or Mac takes them from there instead of translating again.
_REQUIREMENTS = {"requires-darwin": ""}

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
    if info.edits:
        args.add("--edits", info.edits)
    pairs = [delta[DeltaInfo] for delta in ctx.attr.pairs]
    for pair in pairs:
        args.add("--cdhash-pairs", "{}/{}.cdhash-pairs".format(pair.dir.path, pair.role))
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._translate,
        arguments = [args],
        inputs = [ctx.file.image, ctx.file.pins] + info.apple_trustcaches + ([info.edits] if info.edits else []) + [pair.dir for pair in pairs],
        tools = [info.driver_run, info.tool_run],
        outputs = [out],
        mnemonic = "Pretranslate",
        progress_message = "Translating %s's arm64e code to arm64 (%%{label})" % info.role,
        execution_requirements = _REQUIREMENTS,
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

def _pretranslated_updates_impl(ctx):
    out = ctx.actions.declare_directory(ctx.label.name)
    delta = ctx.attr.delta[DeltaInfo]
    args = ctx.actions.args()
    args.add("updates")
    args.add("--delta", delta.dir.path)
    args.add("--role", delta.role)
    args.add("--pinned", ctx.file.pinned)
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._translate,
        arguments = [args],
        inputs = [delta.dir, ctx.file.pinned],
        outputs = [out],
        mnemonic = "PretranslatedUpdates",
        progress_message = "Taking the boot package's translated updates from %{label}'s delta",
    )
    return [DefaultInfo(files = depset([out]))]

pretranslated_updates = rule(
    implementation = _pretranslated_updates_impl,
    doc = "The delta's translations of the files the pinned update list names, at their System paths, and the list: " +
          "what //deploy:sd and //install:sd place under Data's Switch/pretranslated-updates (pretranslate.py).",
    attrs = {
        "delta": attr.label(mandatory = True, providers = [DeltaInfo]),
        "pinned": attr.label(mandatory = True, allow_single_file = True, doc = "ipsw/pretranslated/<kind>/os.updates.json"),
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
        execution_requirements = _REQUIREMENTS,
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

def _delta_metadata_impl(ctx):
    delta = ctx.attr.delta[DeltaInfo]
    out = ctx.actions.declare_directory(ctx.label.name)
    ctx.actions.run_shell(
        command = """set -e
src="$1"
dst="$2"
role="$3"
for name in "${role}.manifest.json" "${role}.trustcache" "${role}.cdhash-pairs" a64x-cache-pretranslated.bin; do
    if [ -f "$src/$name" ]; then
        /bin/cp "$src/$name" "$dst/$name"
    fi
done
""",
        arguments = [delta.dir.path, out.path, delta.role],
        inputs = [delta.dir],
        outputs = [out],
        mnemonic = "DeltaMetadata",
        progress_message = "Extracting %s delta metadata (%%{label})" % delta.role,
    )
    return [DefaultInfo(files = depset([out]))]

delta_metadata = rule(
    implementation = _delta_metadata_impl,
    doc = "The small metadata files of a cryptex delta (manifest, trustcache, cdhash-pairs, cache list), without its " +
          "translated binaries (already sealed into translated_<role>).",
    attrs = {
        "delta": attr.label(mandatory = True, providers = [DeltaInfo]),
    },
)

def _packed_image_impl(ctx):
    out = ctx.actions.declare_directory(ctx.label.name)
    args = ctx.actions.args()
    args.add("--out", out.path)
    args.add("--apfs-tool", ctx.file.apfs_tool.path)
    input_sets = []
    for name, target in sorted(ctx.attr.inputs.items()):
        found = target[DefaultInfo].files.to_list()
        if len(found) > 1:
            fail("{} ({}) must be one file or directory, not {}".format(name, target.label, len(found)))
        if found:
            args.add("--" + name.replace("_", "-"), found[0].path)
            input_sets.append(target[DefaultInfo].files)
            input_sets.append(target[DefaultInfo].default_runfiles.files)
    for extra in ctx.attr.extra_inputs:
        input_sets.append(extra[DefaultInfo].files)
        input_sets.append(extra[DefaultInfo].default_runfiles.files)
    ctx.actions.run(
        executable = ctx.executable._pack,
        arguments = [args],
        inputs = depset(transitive = input_sets),
        tools = [ctx.file.apfs_tool],
        outputs = [out],
        mnemonic = "ImagePack",
        progress_message = "Composing, populating and packing the install image (%{label})",
        execution_requirements = _REQUIREMENTS,
    )
    return [DefaultInfo(files = depset([out]))]

packed_image = rule(
    implementation = _packed_image_impl,
    doc = "Compose, populate, prove fit and pack one iOS version's NAME.SII.* folder as a Bazel action.",
    attrs = {
        "inputs": attr.string_keyed_label_dict(allow_files = True),
        "extra_inputs": attr.label_list(allow_files = True),
        "apfs_tool": attr.label(mandatory = True, cfg = "exec", allow_single_file = True),
        "_pack": attr.label(default = "//tools/install:pack_action", executable = True, cfg = "exec"),
    },
)
