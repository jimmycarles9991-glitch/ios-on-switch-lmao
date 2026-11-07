"""ipsw_free: a release artifact proven, at analysis, to hold no file made from Apple's bytes.

A file is Apple's when it belongs to the IPSW's repositories (@ipsw, @ipsw_<kind>_<role>), to @gpu_apple_libs or to
@gpu_device_data, or when an action that made it read such a file. The aspect follows action inputs, not attribute
edges, so a check that reads the kernelcache and writes only a validation output leaves the kernel clean.
"""

_APPLE_REPOS = ["gpu_apple_libs", "gpu_device_data"]

# The one exception, by owner and path: files made from the IPSW that hold none of its bytes and ship on purpose. The
# native loader compiles in the layout of a kernel window that holds Apple's kexts (sizes, offsets, a hash), which a Mac
# without Xcode cannot compile, so the release carries the loader and constants.json, everything it compiles in; the Mac
# refuses to stage a window whose constants differ (tools/package/bl33.py). Every other output stays Apple's.
DECLASSIFIED = {
    Label("//boot:package"): ["boot/package_work/loader.uboot.bin", "boot/package_work/constants.json"],
}

def is_apple_label(label):
    name = label.repo_name.rpartition("+")[2]
    return name == "ipsw" or name.startswith("ipsw_") or name in _APPLE_REPOS

AppleBytesInfo = provider(
    doc = "The files of a target's closure that hold Apple's bytes, and why each one does; absent when there are none.",
    fields = {
        "files": "depset of File",
        "origins": "depset of (path, reason) tuples, one per file",
    },
)

def _dep_targets(value):
    if type(value) == "Target":
        return [value]
    if type(value) == "list":
        return [v for v in value if type(v) == "Target"]
    if type(value) == "dict":
        return [v for v in value.keys() + value.values() if type(v) == "Target"]
    return []

def _apple_bytes_aspect_impl(target, ctx):
    transitive_files, transitive_origins = [], []
    direct = {}
    for name in dir(ctx.rule.attr):
        for dep in _dep_targets(getattr(ctx.rule.attr, name)):
            if AppleBytesInfo in dep:
                transitive_files.append(dep[AppleBytesInfo].files)
                transitive_origins.append(dep[AppleBytesInfo].origins)
            if is_apple_label(dep.label):
                for f in dep[DefaultInfo].files.to_list():
                    direct[f] = "a file of " + str(dep.label)
    if is_apple_label(target.label):
        for f in target[DefaultInfo].files.to_list():
            direct[f] = "a file of " + str(target.label)

    if not direct and not transitive_files:
        return []

    apple = {f: True for f in depset(direct.keys(), transitive = transitive_files).to_list()}
    declassified = DECLASSIFIED.get(target.label, [])
    pending = list(getattr(target, "actions", []))

    # Actions are not registered in dependency order, so repeat until no new output turns out to be Apple's.
    for _ in range(len(pending)):
        remaining = []
        for action in pending:
            read = [f for f in action.inputs.to_list() if f in apple]
            if not read:
                remaining.append(action)
                continue
            for out in action.outputs.to_list():
                if out not in apple and out.short_path not in declassified:
                    apple[out] = True
                    direct[out] = "{} in {} read {}".format(action.mnemonic, target.label, read[0].path)
        if len(remaining) == len(pending):
            break
        pending = remaining

    return [AppleBytesInfo(
        files = depset(direct.keys(), transitive = transitive_files),
        origins = depset([(f.path, why) for f, why in direct.items()], transitive = transitive_origins),
    )]

apple_bytes_aspect = aspect(
    implementation = _apple_bytes_aspect_impl,
    attr_aspects = ["*"],
    apply_to_generating_rules = True,
    doc = "Collects the files of a target's closure that hold Apple's bytes (AppleBytesInfo).",
)

def _chain(path, reasons):
    lines = []
    for _ in range(64):
        why = reasons.get(path)
        if not why:
            break
        lines.append("  {}: {}".format(path, why))
        path = why.rpartition(" read ")[2] if " read " in why else None
    return "\n".join(lines)

def _ipsw_free_impl(ctx):
    src = ctx.attr.src
    if AppleBytesInfo not in src:
        return [DefaultInfo(files = src[DefaultInfo].files)]
    apple = {f: True for f in src[AppleBytesInfo].files.to_list()}
    tainted = [f for f in src[DefaultInfo].files.to_list() if f in apple]
    if tainted:
        reasons = {path: why for path, why in src[AppleBytesInfo].origins.to_list()}
        fail("{} would ship Apple's bytes (IPSW or device data):\n{}".format(
            src.label,
            "\n".join([_chain(f.path, reasons) for f in tainted]),
        ))
    return [DefaultInfo(files = src[DefaultInfo].files)]

ipsw_free = rule(
    implementation = _ipsw_free_impl,
    doc = "src's files, after checking that none of them was made from the IPSW or the device's data.",
    attrs = {
        "src": attr.label(mandatory = True, allow_files = True, aspects = [apple_bytes_aspect]),
    },
)
