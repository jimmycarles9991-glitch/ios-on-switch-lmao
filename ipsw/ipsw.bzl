"""The IPSWs' members, fetched by HTTP range request (no full IPSW download), one set per IPSW kind (pins.bzl).

Each member of each pinned kind is its own repository (@ipsw_<kind>_<role>), so building something that needs the
16 MB kernelcache never fetches the 5 GB OS image. Bazel downloads exactly the member's stored bytes with a `Range`
header and checks them against the pinned sha256, which puts them in the repository cache (content-addressed, shared
by every output base). The member repositories are reproducible, so the repository contents cache keeps each one once
and every output base's external/ holds a symlink to it.

A stored member (the OS dmg) is used as fetched. A deflated one is inflated by a build action (ipsw_inflate,
//ipsw:inflate) that checks the content against its pin; only the deflated bytes live in the repository cache. An Apple
Encrypted Archive (iOS 26 and later: OS.dmg.aea, the SystemOS cryptex) is decrypted the same way (ipsw_aea_decrypt) with
its pinned aea_key.

@ipsw is the hub the build names members by: @ipsw//:<role> (kernelcache, os, system_cryptex, ...) is the member of
the kind --//ipsw:kind selects (a select() over //ipsw:<kind>). //ipsw:current gives rules the
selected kind's identity and derived pins (IpswInfo). Nothing outside pins.bzl names a build or a member file.

@ipsw//:crawl_tree, :crawl_inventory and :crawl_qc are the selected kind's Metal libraries, crawled once per pin by a
repository (@ipsw_<kind>_crawl, gpu/seed/crawl.bzl) that keeps the crawl and not the images: the shader seed reads
them, so building it never needs the OS image or the cryptexes.
"""

load("@bazel_skylib//lib:selects.bzl", "selects")
load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo", "string_flag")
load("//gpu/seed:crawl.bzl", "QUARTZCORE", "shader_crawl_repository")
load(":pins.bzl", "DEFAULT_KIND", "IPSWS")

# Members above this are inflated only on demand (the SD install): they stay out of `bazel build //...`, out of the
# disk cache, and out of every output base that does not install.
_LARGE = 256 << 20

_STORED_BUILD = """\
exports_files([{name}], visibility = ["//visibility:public"])
"""

# The members a crawl reads, and what the hub names of a crawl repository.
_CRAWLED = ("os", "system_cryptex", "app_cryptex")
_CRAWL_TARGETS = {
    "crawl_inventory": ":inventory.json",
    "crawl_qc": ":tree/" + QUARTZCORE,
    "crawl_tree": ":tree",
}

def pinned_kinds():
    return [kind for kind, ipsw in IPSWS.items() if ipsw.get("members")]

def _filename(pin):
    return pin["member"].split("/")[-1]

def _ipsw_member_impl(rctx):
    first, last = rctx.attr.byte_range.split("-")
    rctx.report_progress("{} (bytes {})".format(rctx.attr.member, rctx.attr.byte_range))
    rctx.download(
        url = rctx.attr.url,
        output = rctx.attr.output,
        sha256 = rctx.attr.sha256,
        headers = {"Range": ["bytes={}-{}".format(first, last)]},
    )
    rctx.file("BUILD.bazel", _STORED_BUILD.format(name = repr(rctx.attr.output)))

    # download() copies the cached member into the repository (only download_and_extract hardlinks). On APFS the copy is
    # a clone, but a clone per output base stops sharing blocks once its repository cache entry is replaced; the
    # contents cache holds one clone, and every output base links to it.
    return rctx.repo_metadata(reproducible = True)

ipsw_member = repository_rule(
    implementation = _ipsw_member_impl,
    attrs = {
        "url": attr.string(mandatory = True),
        "member": attr.string(mandatory = True),
        "byte_range": attr.string(mandatory = True, doc = "first-last byte of the stored member in the IPSW"),
        "output": attr.string(mandatory = True, doc = "the member's file name, or stored.deflate"),
        "sha256": attr.string(mandatory = True, doc = "of the stored bytes"),
    },
)

def _ipsw_hub_impl(rctx):
    lines = ['package(default_visibility = ["//visibility:public"])', ""]
    for role, by_kind in sorted(json.decode(rctx.attr.members).items()):
        lines.append("alias(")
        lines.append('    name = "{}",'.format(role))
        lines.append("    actual = select({")
        for kind, actual in sorted(by_kind.items()):
            lines.append('        "@@//ipsw:{}": "{}",'.format(kind, actual))
        lines.append("    }),")
        lines.append(")")
        lines.append("")

    # The deflated members' stored bytes, by kind, for //ipsw's inflate actions.
    for name, actual in sorted(rctx.attr.stored.items()):
        lines.append('alias(name = "{}", actual = "{}")'.format(name, actual))
    for name, target in sorted(_CRAWL_TARGETS.items()):
        lines.append("")
        lines.append("alias(")
        lines.append('    name = "{}",'.format(name))
        lines.append("    actual = select({")
        for kind, repo in sorted(rctx.attr.crawls.items()):
            actual = "@{}//{}".format(repo, target) if repo else "@@//ipsw:{}_unpinned".format(kind)
            lines.append('        "@@//ipsw:{}": "{}",'.format(kind, actual))
        lines.append("    }),")
        lines.append(")")
    rctx.file("BUILD.bazel", "\n".join(lines) + "\n")

_ipsw_hub = repository_rule(
    implementation = _ipsw_hub_impl,
    attrs = {
        "members": attr.string(doc = "JSON: role -> {kind -> label of its file}"),
        "stored": attr.string_dict(doc = "stored_<kind>_<role> -> the deflated member's stored bytes"),
        "crawls": attr.string_dict(doc = "kind -> its crawl repository, or empty where the kind lacks a crawled member"),
    },
)

def _ipsw_extension_impl(module_ctx):
    roles = {}
    for ipsw in IPSWS.values():
        for role in ipsw.get("members", {}):
            roles[role] = {}
    stored = {}
    for kind, ipsw in IPSWS.items():
        for role in roles:
            pin = ipsw.get("members", {}).get(role)
            if not pin:
                # A declared slot, or a kind without this member: analysis of anything that reads it fails, with why.
                roles[role][kind] = "@@//ipsw:{}_unpinned".format(kind)
                continue
            repo = "ipsw_{}_{}".format(kind, role)
            ipsw_member(
                name = repo,
                url = ipsw["url"],
                member = pin["member"],
                byte_range = "{}-{}".format(pin["offset"], pin["offset"] + pin["stored_size"] - 1),
                output = "stored.deflate" if pin["deflated"] else _filename(pin),
                sha256 = pin["sha256"],
            )
            if pin["deflated"]:
                roles[role][kind] = "@@//ipsw:{}_{}".format(kind, role)
                stored["stored_{}_{}".format(kind, role)] = "@{}//:stored.deflate".format(repo)
            elif pin.get("aea_key"):
                roles[role][kind] = "@@//ipsw:{}_{}".format(kind, role)
                stored["stored_{}_{}".format(kind, role)] = "@{}//:{}".format(repo, _filename(pin))
            else:
                roles[role][kind] = "@{}//:{}".format(repo, _filename(pin))
    crawls = {}
    for kind, ipsw in IPSWS.items():
        members = ipsw.get("members", {})
        crawls[kind] = ""
        if [role for role in _CRAWLED if not members.get(role)]:
            continue
        crawls[kind] = "ipsw_{}_crawl".format(kind)
        shader_crawl_repository(
            name = crawls[kind],
            url = ipsw["url"],
            pins = json.encode({role: members[role] for role in _CRAWLED}),
        )
    _ipsw_hub(name = "ipsw", members = json.encode(roles), stored = stored, crawls = crawls)
    return module_ctx.extension_metadata(reproducible = True)

ipsw = module_extension(implementation = _ipsw_extension_impl)

def ipsw_member_label(kind, role):
    """The file of a stored (not deflated) member of a pinned kind, for repository rules (which cannot select())."""
    pin = IPSWS[kind]["members"][role]
    if pin["deflated"] or pin.get("aea_key"):
        fail("{} {} is deflated or encrypted: only a build action makes its content".format(kind, role))
    return Label("@@+ipsw+ipsw_{}_{}//:{}".format(kind, role, _filename(pin)))

# --- The kind, and what rules read of it ----------------------------------------------------------------------------

_UNPINNED = ("IPSW kind {kind} is declared in ipsw/pins.bzl but not pinned (or lacks this member): " +
             "bazel run //ipsw:pin -- --kind=<its build> --replace={kind} <ipsw url> <product> <device class>")

IpswInfo = provider(
    doc = "The selected IPSW kind: its identity (pins.bzl, without the members) and derived pins.",
    fields = {
        "kind": "the kind's name (its build)",
        "build": "ProductBuildVersion",
        "version": "ProductVersion",
        "product": "the product type the members are for",
        "device_class": "the build identity's device class",
        "derived": "dict: the pins of what the build derives from the members",
        "pins": "the kind's pins.bzl entry without derived (what //install checks Preboot copies against)",
    },
)

def _ipsw_info_impl(ctx):
    kind = ctx.attr._kind[BuildSettingInfo].value
    ipsw = IPSWS[kind]
    if not ipsw.get("members"):
        fail(_UNPINNED.format(kind = kind))
    return [IpswInfo(
        kind = kind,
        build = ipsw["build"],
        version = ipsw["version"],
        product = ipsw["product"],
        device_class = ipsw["device_class"],
        derived = ipsw.get("derived", {}),
        pins = {k: v for k, v in ipsw.items() if k != "derived"},
    )]

ipsw_info = rule(
    implementation = _ipsw_info_impl,
    doc = "IpswInfo of the kind --//ipsw:kind selects.",
    attrs = {"_kind": attr.label(default = "//ipsw:kind")},
)

def _ipsw_unpinned_impl(ctx):
    fail(_UNPINNED.format(kind = ctx.attr.kind))

_ipsw_unpinned = rule(implementation = _ipsw_unpinned_impl, attrs = {"kind": attr.string()})

# inflate.py: inflating on one core, hashing and writing on another.
def _inflate_resources(_os, _inputs):
    return {"cpu": 2, "memory": 512}

def _ipsw_inflate_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + "/" + ctx.attr.filename)
    ctx.actions.run(
        executable = ctx.executable._inflate,
        arguments = [ctx.file.src.path, out.path, ctx.attr.content_sha256],
        inputs = [ctx.file.src],
        outputs = [out],
        mnemonic = "IpswInflate",
        progress_message = "Inflating the IPSW's " + ctx.attr.filename,
        # A multi-GB output (the system cryptex) would evict everything else from the 10 GB disk cache, and its input,
        # from the repository cache, is no remote worker's to read.
        execution_requirements = {"no-cache": "", "no-remote": ""} if ctx.attr.large else {},
        resource_set = _inflate_resources,
    )
    return [DefaultInfo(files = depset([out]))]

ipsw_inflate = rule(
    implementation = _ipsw_inflate_impl,
    doc = "A deflated IPSW member's content (<name>/<filename>), checked against its content sha256.",
    attrs = {
        "src": attr.label(mandatory = True, allow_single_file = True, doc = "the member's stored (deflated) bytes"),
        "filename": attr.string(mandatory = True),
        "content_sha256": attr.string(mandatory = True),
        "large": attr.bool(doc = "kept out of the disk and remote caches"),
        "_inflate": attr.label(default = "//ipsw:inflate", executable = True, cfg = "exec"),
    },
)

def _ipsw_aea_decrypt_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + "/" + ctx.attr.filename)
    ctx.actions.run(
        executable = ctx.executable._aea_decrypt,
        arguments = [ctx.file.src.path, out.path, ctx.attr.key, ctx.attr.content_sha256],
        inputs = [ctx.file.src],
        outputs = [out],
        mnemonic = "IpswAeaDecrypt",
        progress_message = "Decrypting the IPSW's " + ctx.attr.filename,
        # A multi-GB member of the repository cache: decrypted here, never uploaded for a remote worker.
        execution_requirements = {"no-cache": "", "no-remote": "", "requires-darwin": ""},
    )
    return [DefaultInfo(files = depset([out]))]

ipsw_aea_decrypt = rule(
    implementation = _ipsw_aea_decrypt_impl,
    doc = "An AEA member's content (<name>/<filename>), decrypted by /usr/bin/aea and checked against its content sha256.",
    attrs = {
        "content_sha256": attr.string(mandatory = True),
        "filename": attr.string(mandatory = True),
        "key": attr.string(mandatory = True, doc = "aea decrypt's -key-value"),
        "src": attr.label(mandatory = True, allow_single_file = True, doc = "the member's stored archive"),
        "_aea_decrypt": attr.label(default = "//ipsw:aea_decrypt", executable = True, cfg = "exec"),
    },
)

def kinds_where(field, value):
    """The kinds whose pins.bzl entry has field == value.

    Args:
        field: a key of a kind's entry ("ios", "xnu").
        value: the value it must have.

    Returns:
        The kinds, sorted.
    """
    return sorted([kind for kind, ipsw in IPSWS.items() if ipsw.get(field) == value])

def ios_version(kind):
    """The kind's iOS major version (pins.bzl's ios).

    Args:
        kind: an IPSW kind.

    Returns:
        The version, an int.
    """
    return int(IPSWS[kind]["ios"])

def kernel_virt_base(kind):
    """The kind's pinned kernel_virt_base (pins.bzl's derived): the prelinked kernel's lowest segment, as hex.

    Args:
        kind: an IPSW kind.

    Returns:
        The base, a "0x..." string.
    """
    base = IPSWS[kind].get("derived", {}).get("kernel_virt_base")
    if not base:
        fail("IPSW kind {} has no derived kernel_virt_base in ipsw/pins.bzl".format(kind))
    return base

def os_volume_uuid(kind):
    """The kind's System volume UUID (pins.bzl's "volumes"): what /chosen/root-matching names.

    Args:
        kind: an IPSW kind.

    Returns:
        The UUID, upper case.
    """
    volumes = IPSWS[kind].get("volumes")
    if not volumes:
        fail("IPSW kind {} has no \"volumes\" in ipsw/pins.bzl".format(kind))
    return volumes["os_volume_uuid"].upper()

# Where the image builder stages the cryptex images on Preboot (tools/install/stages.py STAGING).
_CRYPTEX_STAGING = "/private/preboot/Switch-Staging/"

def _ipsw_volumes_header_impl(ctx):
    kind = ctx.attr._kind[BuildSettingInfo].value
    volumes = IPSWS[kind].get("volumes")
    if not volumes:
        fail("IPSW kind {} has no \"volumes\" in ipsw/pins.bzl".format(kind))
    lines = ["// IPSW kind {}'s build and volumes (ipsw/pins.bzl), as the image builder lays them out.".format(kind)]
    lines += ctx.attr.lines
    lines.append("#define SWITCH_IPSW_BUILD \"{}\"".format(IPSWS[kind]["build"]))

    # The preprocessor cannot compare strings: per-kind data selects on these.
    lines.append("#define SWITCH_IPSW_KIND_{} 1".format(IPSWS[kind]["build"]))
    lines.append("#define SWITCH_IPSW_DEVICE_{} 1".format(IPSWS[kind]["device_class"].upper()))
    for macro, key in (
        ("SWITCH_OS_VOLUME_UUID", "os_volume_uuid"),
        ("SWITCH_SYSTEM_CRYPTEX_UUID", "system_cryptex_uuid"),
        ("SWITCH_APP_CRYPTEX_UUID", "app_cryptex_uuid"),
    ):
        lines.append("#define {} \"{}\"".format(macro, volumes[key].upper()))
    for macro, key in (
        ("SWITCH_SYSTEM_CRYPTEX_IMAGE", "system_cryptex_image"),
        ("SWITCH_APP_CRYPTEX_IMAGE", "app_cryptex_image"),
    ):
        lines.append("#define {} \"{}{}\"".format(macro, _CRYPTEX_STAGING, volumes[key]))
    ctx.actions.write(ctx.outputs.out, "\n".join(lines) + "\n")

ipsw_volumes_header = rule(
    implementation = _ipsw_volumes_header_impl,
    doc = "A C header naming the selected kind's build (SWITCH_IPSW_BUILD; SWITCH_IPSW_KIND_<build> and " +
          "SWITCH_IPSW_DEVICE_<DEVICE CLASS> as 1 for #if), System volume and cryptexes " +
          "(SWITCH_OS_VOLUME_UUID, SWITCH_{SYSTEM,APP}_CRYPTEX_{UUID,IMAGE}): what the kernel finds and mounts at boot.",
    attrs = {
        "out": attr.output(mandatory = True),
        "lines": attr.string_list(doc = "lines written before the volumes'"),
        "_kind": attr.label(default = "//ipsw:kind"),
    },
)

def config_setting_any(name, settings, visibility):
    """A config_setting matching any of settings.

    Args:
        name: the setting's name.
        settings: one or more labels of config_settings.
        visibility: its visibility.
    """
    if len(settings) == 1:
        native.alias(name = name, actual = settings[0], visibility = visibility)
    else:
        selects.config_setting_group(name = name, match_any = settings, visibility = visibility)

def ipsw_kinds():  # buildifier: disable=unnamed-macro
    """The kind flag and what selects on it, in //ipsw.

    A config_setting per kind and per iOS major (ios<major>), the deflated members inflated (<kind>_<role>), the
    unpinned stand-ins (<kind>_unpinned) and :current (IpswInfo).
    """
    string_flag(
        name = "kind",
        build_setting_default = DEFAULT_KIND,
        values = list(IPSWS),
        visibility = ["//visibility:public"],
    )
    roles = {role: True for ipsw in IPSWS.values() for role in ipsw.get("members", {})}
    for kind, ipsw in IPSWS.items():
        native.config_setting(
            name = kind,
            flag_values = {":kind": kind},
            visibility = ["//visibility:public"],
        )
        if [role for role in roles if role not in ipsw.get("members", {})]:
            # What @ipsw//:<role> selects for a declared slot (or a kind without that member): analysis fails with
            # the pin command. Manual, so `bazel build //...` does not analyze it.
            _ipsw_unpinned(name = kind + "_unpinned", kind = kind, tags = ["manual"], visibility = ["//visibility:public"])
        for role, pin in ipsw.get("members", {}).items():
            if pin.get("aea_key"):
                ipsw_aea_decrypt(
                    name = "{}_{}".format(kind, role),
                    src = "@ipsw//:stored_{}_{}".format(kind, role),
                    filename = _filename(pin).removesuffix(".aea"),
                    key = pin["aea_key"],
                    content_sha256 = pin["content_sha256"],
                    tags = ["manual"],
                    visibility = ["//visibility:public"],
                )
                continue
            if not pin["deflated"]:
                continue
            large = pin["stored_size"] > _LARGE
            ipsw_inflate(
                name = "{}_{}".format(kind, role),
                src = "@ipsw//:stored_{}_{}".format(kind, role),
                filename = _filename(pin),
                content_sha256 = pin["content_sha256"],
                large = large,
                # Only the SD install reads the large ones: `bazel build //...` leaves them alone.
                tags = ["manual"] if large else [],
                visibility = ["//visibility:public"],
            )
    for major in sorted({ipsw["ios"]: True for ipsw in IPSWS.values() if ipsw.get("ios")}):
        config_setting_any(
            name = "ios" + major,
            settings = [":" + kind for kind in kinds_where("ios", major)],
            visibility = ["//visibility:public"],
        )
    ipsw_info(name = "current", visibility = ["//visibility:public"])
