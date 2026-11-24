"""Build a Cortex-R4 bootstrap in its bounded persistent firmware island, and the ELF images its CPU tests run."""

_TOOLCHAIN_ATTRS = {
    "_gcc": attr.label(default = "@arm_none_eabi//:bin/arm-none-eabi-gcc", executable = True, allow_single_file = True, cfg = "exec"),
    "_objcopy": attr.label(default = "@arm_none_eabi//:bin/arm-none-eabi-objcopy", executable = True, allow_single_file = True, cfg = "exec"),
    "_toolchain": attr.label(default = "@arm_none_eabi//:all"),
}

def _link(ctx, elf, flags, srcs, inputs):
    args = ctx.actions.args()
    args.add_all(flags)
    args.add_all(srcs)
    args.add("-o", elf)
    ctx.actions.run(
        executable = ctx.executable._gcc,
        arguments = [args],
        inputs = inputs,
        tools = ctx.files._toolchain,
        outputs = [elf],
        mnemonic = "WlanA3Link",
        env = {"PATH": "/usr/bin:/bin"},
    )

def _payload_impl(ctx):
    elf = ctx.actions.declare_file(ctx.label.name + ".elf")
    flags = [
        "-mcpu=cortex-r4",
        "-mthumb",
        "-mfloat-abi=soft",
        "-mno-unaligned-access",
        "-Os",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables",
        "-ffunction-sections",
        "-fdata-sections",
        "-nostdlib",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wl,--build-id=none,--gc-sections,--fatal-warnings,--no-warn-rwx-segments",
    ]
    flags.append("-DDEVELOPMENT=" + ("1" if ctx.attr.development else "0"))
    if ctx.attr.lto:
        flags.append("-flto")
    if ctx.attr.candidate:
        flags.append("-DSWITCH_A3_CANDIDATE=1")
        flags.extend([
            "-fno-caller-saves",
            "-fno-partial-inlining",
            "-fno-schedule-insns2",
            "-fno-tree-loop-optimize",
            "-fno-optimize-sibling-calls",
            "-fno-ipa-bit-cp",
            "-fno-tree-vrp",
        ])
    flags.extend(["-Wl,--undefined=" + symbol for symbol in ctx.attr.retain_symbols])
    flags.extend(["-T", ctx.file.link_script.path])
    _link(ctx, elf, flags, ctx.files.srcs, ctx.files.srcs + ctx.files.hdrs + [ctx.file.link_script])
    binary = ctx.actions.declare_file(ctx.label.name + ".bin")
    ctx.actions.run(
        executable = ctx.executable._objcopy,
        arguments = ["-O", "binary", elf.path, binary.path],
        inputs = [elf],
        tools = ctx.files._toolchain,
        outputs = [binary],
        mnemonic = "WlanA3Objcopy",
    )
    header = ctx.outputs.header
    ctx.actions.run(
        executable = ctx.executable._header,
        arguments = [elf.path, binary.path, header.path] + (["--candidate"] if ctx.attr.candidate else []),
        inputs = [elf, binary],
        outputs = [header],
        mnemonic = "WlanA3Header",
    )
    return [
        DefaultInfo(files = depset([header])),
        OutputGroupInfo(elf = depset([elf]), binary = depset([binary])),
    ]

switch_wlan_a3_payload = rule(
    implementation = _payload_impl,
    attrs = _TOOLCHAIN_ATTRS | {
        "srcs": attr.label_list(allow_files = [".c", ".S"], mandatory = True),
        "hdrs": attr.label_list(allow_files = [".h"]),
        "link_script": attr.label(allow_single_file = True, mandatory = True),
        "header": attr.output(mandatory = True),
        "development": attr.bool(default = False),
        "retain_symbols": attr.string_list(),
        "lto": attr.bool(default = False),
        "candidate": attr.bool(default = False),
        "_header": attr.label(default = ":payload_header", executable = True, cfg = "exec"),
    },
)

def _test_image_impl(ctx):
    elf = ctx.actions.declare_file(ctx.label.name + ".elf")
    flags = ctx.attr.copts + ["-Wa,-I" + file.dirname for file in ctx.files.incbin]
    _link(ctx, elf, flags, ctx.files.srcs, ctx.files.srcs + ctx.files.hdrs + ctx.files.incbin)
    return [DefaultInfo(files = depset([elf]))]

switch_wlan_a3_test_image = rule(
    implementation = _test_image_impl,
    doc = "One gcc compile and link of a CPU test's sources, in srcs order, into <name>.elf.",
    attrs = _TOOLCHAIN_ATTRS | {
        "srcs": attr.label_list(allow_files = [".c", ".S"], mandatory = True),
        "hdrs": attr.label_list(allow_files = [".h"]),
        "copts": attr.string_list(mandatory = True),
        "incbin": attr.label_list(allow_files = True, doc = "Files the assembler's .incbin reads, found through -Wa,-I."),
    },
)
