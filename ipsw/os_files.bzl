"""os_files_repository: small pinned files extracted once from one IPSW kind's OS image (ipsw/os_files.py).

Fetching and mounting the 5-8 GB OS image inside a reproducible repository rule keeps only the ~3 MB of files that
build actions actually read (SystemVersion.plist, installd, launchd.plist/.sig, migration donor binaries). Build
actions never take the multi-gigabyte OS image as an input, never mount disk images and never upload gigabytes to a
remote cache.

No loads: everything this file loads is part of the repository contents cache key, and pins.bzl changes whenever any
kind is pinned.
"""

_BUILD = """\
package(default_visibility = ["//visibility:public"])

exports_files([
{exports}])

filegroup(
    name = "migration_files",
    srcs = glob([
        "files/System/Library/CoreServices/SystemVersion.plist",
        "files/System/Library/DataClassMigrators/**",
        "files/System/Library/PrivateFrameworks/DataMigration.framework/**",
    ]),
)
"""

def _fail(rctx, result, what):
    if result.return_code:
        fail("{}: {} failed:\n{}{}".format(rctx.name, what, result.stdout[-4000:], result.stderr[-4000:]))

def _os_image(rctx, python, pin):
    stored = "images/os.stored"
    content = "images/os.dmg"
    plain = not pin["deflated"] and not pin.get("aea_key")
    first = pin["offset"]
    last = first + pin["stored_size"] - 1
    rctx.report_progress("os: fetching {} ({} MB)".format(pin["member"], pin["stored_size"] >> 20))
    rctx.download(
        url = rctx.attr.url,
        output = content if plain else stored,
        sha256 = pin["sha256"],
        headers = {"Range": ["bytes={}-{}".format(first, last)]},
    )
    if plain:
        return content
    if pin.get("aea_key"):
        rctx.report_progress("os: decrypting {}".format(pin["member"]))
        command = [python, rctx.path(rctx.attr._aea_decrypt), stored, content, pin["aea_key"], pin["content_sha256"]]
    else:
        rctx.report_progress("os: inflating {}".format(pin["member"]))
        command = [python, rctx.path(rctx.attr._inflate), stored, content, pin["content_sha256"]]
    _fail(rctx, rctx.execute(command, timeout = 3600, quiet = True), "making os")
    rctx.delete(stored)
    return content

def _os_files_repository_impl(rctx):
    python = str(rctx.path(rctx.attr._python))
    rctx.watch(rctx.attr._extractor)
    rctx.watch(rctx.attr._inflate)
    rctx.watch(rctx.attr._aea_decrypt)
    pin = json.decode(rctx.attr.pin)
    image = _os_image(rctx, python, pin)
    rctx.report_progress("extracting build files from {}".format(pin["member"]))
    command = [
        python,
        str(rctx.path(rctx.attr._extractor)),
        "--image",
        image,
        "--volume-uuid",
        rctx.attr.volume_uuid,
        "--build",
        rctx.attr.build,
        "--out",
        "files",
    ]
    if rctx.attr.launchd_catalog_sha256:
        command += [
            "--launchd-catalog-sha256",
            rctx.attr.launchd_catalog_sha256,
            "--launchd-catalog-signature-sha256",
            rctx.attr.launchd_catalog_signature_sha256,
        ]
    result = rctx.execute(command, timeout = 600)
    rctx.delete("images")
    _fail(rctx, result, "extracting os files")
    exports = [
        "files/System/Library/CoreServices/SystemVersion.plist",
        "files/usr/libexec/installd",
    ]
    if rctx.attr.launchd_catalog_sha256:
        exports += [
            "files/System/Library/xpc/launchd.plist",
            "files/System/Library/xpc/launchd.plist.sig",
        ]
    rctx.file(
        "BUILD.bazel",
        _BUILD.format(exports = "".join(['    "{}",\n'.format(path) for path in exports])),
    )
    return rctx.repo_metadata(reproducible = True)

os_files_repository = repository_rule(
    implementation = _os_files_repository_impl,
    doc = "Small pinned files extracted from one IPSW kind's OS volume image.",
    attrs = {
        "url": attr.string(mandatory = True, doc = "the IPSW"),
        "pin": attr.string(mandatory = True, doc = "JSON: the os member's pins.bzl entry"),
        "build": attr.string(mandatory = True, doc = "pinned ProductBuildVersion"),
        "volume_uuid": attr.string(mandatory = True, doc = "pinned System APFS volume UUID"),
        "launchd_catalog_sha256": attr.string(default = ""),
        "launchd_catalog_signature_sha256": attr.string(default = ""),
        "_python": attr.label(default = Label("@python_3_13_host//:python")),
        "_extractor": attr.label(default = Label("//ipsw:os_files.py")),
        "_inflate": attr.label(default = Label("//ipsw:inflate.py")),
        "_aea_decrypt": attr.label(default = Label("//ipsw:aea_decrypt.py")),
    },
)
