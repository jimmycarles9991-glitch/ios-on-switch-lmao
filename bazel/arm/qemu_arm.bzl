"""@qemu_arm: xPack's QEMU 9.2.4 build for Apple silicon, which runs the Wi-Fi chip's Cortex-R4 code in the CPU tests.

Only qemu-system-arm and the dylibs it loads from @loader_path/../libexec are extracted (38 MB of the release's 400).
To move to a newer release, take the URL and the sha256 from its .sha file on the xPack release page.
"""

RELEASE = {
    "url": "https://github.com/xpack-dev-tools/qemu-arm-xpack/releases/download/v9.2.4-1/xpack-qemu-arm-9.2.4-1-darwin-arm64.tar.gz",
    "sha256": "2752ae3bdff112a8574373157ef1e0eb51fd50b1ee8554275f2ff20ba1ff1b6d",
    "prefix": "xpack-qemu-arm-9.2.4-1",
}

_BUILD = """package(default_visibility = ["//visibility:public"])

filegroup(
    name = "qemu-system-arm",
    srcs = ["bin/qemu-system-arm"],
    data = glob(["libexec/**"]),
)
"""

def _qemu_arm_impl(rctx):
    if rctx.os.name != "mac os x" or rctx.os.arch != "aarch64":
        fail("@qemu_arm is pinned for Apple silicon Macs")
    archive = "qemu.tar.gz"
    rctx.download(url = RELEASE["url"], output = archive, sha256 = RELEASE["sha256"])
    members = [RELEASE["prefix"] + "/" + m for m in ("bin/qemu-system-arm", "libexec")]
    result = rctx.execute(["/usr/bin/tar", "-xzf", archive, "--strip-components", "1"] + members)
    if result.return_code:
        fail("extracting QEMU failed: " + result.stderr)
    rctx.delete(archive)
    rctx.file("BUILD.bazel", _BUILD)
    return rctx.repo_metadata(reproducible = True)

qemu_arm = repository_rule(implementation = _qemu_arm_impl)
