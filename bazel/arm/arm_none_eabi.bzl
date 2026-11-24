"""@arm_none_eabi: the Arm GNU Toolchain for the BPMP's ARM7TDMI (armv4t Thumb), which hekate payloads are built for,
and for the Wi-Fi chip's Cortex-R4 payload and its CPU tests.

Only the C compiler, assembler, linker, objcopy, nm, objdump, libgcc and newlib's libc for thumb/nofp are extracted
(67 MB of the release's 1.3 GB).
"""

RELEASE = {
    "url": "https://developer.arm.com/-/media/Files/downloads/gnu/14.3.rel1/binrel/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi.tar.xz",
    "sha256": "30f4d08b219190a37cded6aa796f4549504902c53cfc3c7e044a8490b6eba1f7",
    "prefix": "arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi",
    "gcc": "14.3.1",
}

MEMBERS = [
    "bin/arm-none-eabi-gcc",
    "bin/arm-none-eabi-nm",
    "bin/arm-none-eabi-objcopy",
    "bin/arm-none-eabi-objdump",
    "libexec/gcc/arm-none-eabi/{gcc}/cc1",
    "libexec/gcc/arm-none-eabi/{gcc}/collect2",
    "libexec/gcc/arm-none-eabi/{gcc}/lto-wrapper",
    "libexec/gcc/arm-none-eabi/{gcc}/lto1",
    "libexec/gcc/arm-none-eabi/{gcc}/liblto_plugin.so",
    "lib/gcc/arm-none-eabi/{gcc}/include",
    "lib/gcc/arm-none-eabi/{gcc}/include-fixed",
    "lib/gcc/arm-none-eabi/{gcc}/thumb/nofp/libgcc.a",
    "arm-none-eabi/bin/as",
    "arm-none-eabi/bin/ld",
    "arm-none-eabi/include",
    # libc.a is a hard link to libg.a in the archive: both or neither.
    "arm-none-eabi/lib/thumb/nofp/libg.a",
    "arm-none-eabi/lib/thumb/nofp/libc.a",
]

_BUILD = """package(default_visibility = ["//visibility:public"])

exports_files(["bin/arm-none-eabi-gcc", "bin/arm-none-eabi-nm", "bin/arm-none-eabi-objcopy", "bin/arm-none-eabi-objdump"])

filegroup(
    name = "all",
    srcs = glob(["**"], exclude = ["BUILD.bazel", "WORKSPACE"]),
)
"""

def _arm_none_eabi_impl(rctx):
    if rctx.os.name != "mac os x" or rctx.os.arch != "aarch64":
        fail("@arm_none_eabi is pinned for Apple silicon Macs; take install-ios.bin from the release elsewhere")
    archive = "toolchain.tar.xz"
    rctx.download(url = RELEASE["url"], output = archive, sha256 = RELEASE["sha256"])
    members = [RELEASE["prefix"] + "/" + m.format(gcc = RELEASE["gcc"]) for m in MEMBERS]
    result = rctx.execute(["/usr/bin/tar", "-xJf", archive, "--strip-components", "1"] + members)
    if result.return_code:
        fail("extracting the Arm GNU Toolchain failed: " + result.stderr)
    rctx.delete(archive)
    rctx.file("BUILD.bazel", _BUILD)
    return rctx.repo_metadata(reproducible = True)

arm_none_eabi = repository_rule(implementation = _arm_none_eabi_impl)
