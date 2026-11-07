"""shader_crawl_repository: one IPSW kind's Metal libraries, crawled once (tools/gpu/shaders/crawl.py); the ipsw
extension makes one per pinned kind (@ipsw_<kind>_crawl, @ipsw//:crawl_*).

The repository fetches the kind's OS image and cryptexes by range request, makes their content (inflated, decrypted),
crawls it and keeps only the crawl: tree/root, tree/data and inventory.json, about 233 MB of the 6.5 GB (7.7 GB OS image
alone for iOS 27) it read. The images bypass the repository cache (no sha256 given to the download; it is checked after)
and are deleted, so a Mac that only builds the seed never keeps them; //install's image builds fetch them through @ipsw.

Reproducible: Bazel keeps the result in its repository contents cache, keyed by the pins, this file and the crawler's
sources, and hands it to every output base. Any of those changing fetches and crawls again.

No loads: everything this file loads is part of that key, and pins.bzl changes whenever any kind is pinned.
"""

_BUILD = """\
package(default_visibility = ["//visibility:public"])

filegroup(
    name = "tree",
    srcs = glob(["tree/**"]),
)

exports_files([
    "inventory.json",
    "tree/{qc}",
])
"""

QUARTZCORE = "root/System/Library/Frameworks/QuartzCore.framework/default.metallib"

def _fail(rctx, result, what):
    if result.return_code:
        fail("{}: {} failed:\n{}{}".format(rctx.name, what, result.stdout[-4000:], result.stderr[-4000:]))

def _image(rctx, python, role, pin):
    """Fetch one member and make its content at images/<role>.dmg; return that path."""
    stored = "images/{}.stored".format(role)
    content = "images/{}.dmg".format(role)
    plain = not pin["deflated"] and not pin.get("aea_key")
    first = pin["offset"]
    last = first + pin["stored_size"] - 1
    rctx.report_progress("{}: fetching {} ({} MB)".format(role, pin["member"], pin["stored_size"] >> 20))
    fetched = rctx.download(
        url = rctx.attr.url,
        output = content if plain else stored,
        headers = {"Range": ["bytes={}-{}".format(first, last)]},
    )
    if fetched.sha256 != pin["sha256"]:
        fail("{}: {} fetched as sha256 {}, pinned {}".format(rctx.name, pin["member"], fetched.sha256, pin["sha256"]))
    if plain:
        return content
    if pin.get("aea_key"):
        rctx.report_progress("{}: decrypting {}".format(role, pin["member"]))
        command = [python, rctx.path(rctx.attr._aea_decrypt), stored, content, pin["aea_key"], pin["content_sha256"]]
    else:
        rctx.report_progress("{}: inflating {}".format(role, pin["member"]))
        command = [python, rctx.path(rctx.attr._inflate), stored, content, pin["content_sha256"]]
    _fail(rctx, rctx.execute(command, timeout = 3600, quiet = True), "making " + role)
    rctx.delete(stored)
    return content

def _shader_crawl_repository_impl(rctx):
    python = str(rctx.path(rctx.attr._python))
    pins = json.decode(rctx.attr.pins)
    for source in rctx.attr._crawler:
        rctx.symlink(rctx.path(source), "crawler/tools/gpu/shaders/" + source.name)
    images = [_image(rctx, python, role, pins[role]) for role in ("os", "system_cryptex", "app_cryptex")]
    rctx.report_progress("crawling the images for Metal libraries")
    command = [python, "-m", "tools.gpu.shaders.crawl", "--out", "tree", "--inventory", "inventory.json"]
    for image in images:
        command += ["--image", image]
    result = rctx.execute(command, timeout = 3600, environment = {"PYTHONPATH": str(rctx.path("crawler"))})
    rctx.delete("images")
    rctx.delete("crawler")
    _fail(rctx, result, "the crawl")
    if not rctx.path("tree/" + QUARTZCORE).exists:
        fail("{}: the crawl found no {}".format(rctx.name, QUARTZCORE))
    rctx.file("BUILD.bazel", _BUILD.format(qc = QUARTZCORE))
    return rctx.repo_metadata(reproducible = True)

shader_crawl_repository = repository_rule(
    implementation = _shader_crawl_repository_impl,
    doc = "An IPSW kind's crawled Metal libraries: tree/ (root/, data/) and inventory.json.",
    attrs = {
        "url": attr.string(mandatory = True, doc = "the IPSW"),
        "pins": attr.string(mandatory = True, doc = "JSON: os, system_cryptex, app_cryptex -> its pins.bzl member pin"),
        "_python": attr.label(default = Label("@python_3_13_host//:python")),
        "_crawler": attr.label_list(default = [Label("//tools/gpu/shaders:crawl.py"), Label("//tools/gpu/shaders:metallib.py")]),
        "_inflate": attr.label(default = Label("//ipsw:inflate.py")),
        "_aea_decrypt": attr.label(default = Label("//ipsw:aea_decrypt.py")),
    },
)
