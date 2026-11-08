"""Flag lists several kernel drivers share (moved out of the kernel tree's BUILD.bazel by the layout move)."""

# The selected IPSW kind's header (//kernel:ipsw_volumes_header) in every source: its build, iPad and volumes.
IPSW_KIND_FLAGS = [
    "-include",
    "{build}/ipsw-volumes/switch_ipsw_volumes.h",
]

# NativeSwitchBlock: the SD card's block device (+ cryptex files), and the board drivers riding in its archive.
BLOCK_FLAGS = [
    "-mgeneral-regs-only",
    "-mstrict-align",
    "-I",
    "{build}/storage-abi/include",
    "-I",
    "{tree}/diagnostics/switch/storage",
    "-I",
    "{tree}/diagnostics/switch",
    "-I",
    "{tree}/kits/VerifiedRoot/include",
] + IPSW_KIND_FLAGS

# TegraGM20B: the Tegra X1 GPU (vendored nouveau nvkm under an XNU shim, IOAcceleratorES + AGXDeviceUserClient). Apple
# clang's kernel defaults add -Wsign-conversion, -Wshorten-64-to-32 and
# -Wimplicit-int-conversion; Linux-style nvkm code and its shim trip them over a thousand times.
GPU_C_FLAGS = [
    "-mgeneral-regs-only",
    "-mstrict-align",
    "-fno-stack-protector",
    "-std=gnu11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wno-unused-parameter",
    "-Wno-sign-conversion",
    "-Wno-shorten-64-to-32",
    "-Wno-implicit-int-conversion",
    "-I",
    "{tree}/drivers/TegraGM20B/shim/include",
    "-I",
    "{tree}/drivers/TegraGM20B/nouveau/include",
    "-I",
    "{tree}/drivers/TegraGM20B/nouveau/include/nvkm",
    "-I",
    "{tree}/drivers/TegraGM20B/nouveau/nvkm",
    "-I",
    "{tree}/drivers/TegraGM20B/nouveau/nvkm/subdev/gsp",
    "-I",
    "{tree}/drivers/TegraGM20B/nouveau",
    "-include",
    "{tree}/drivers/TegraGM20B/shim/include/nvxnu_prelude.h",
]

# The relaxations Linux builds vendored code with (and two Apple clang defaults upstream code trips).
LINUX_RELAXATIONS = [
    "-Wno-pointer-sign",
    "-Wno-sign-compare",
    "-Wno-missing-field-initializers",
    "-Wno-shadow",
    "-Wno-conditional-uninitialized",
]

# TegraDFLL: T210B01 CPU DFLL (closed-loop CL-DVFS), L4T clk-dfll.c + fcpu/cvb/speedo under a Linux shim.
DFLL_C_FLAGS = [
    "-mgeneral-regs-only",
    "-mstrict-align",
    "-fno-stack-protector",
    "-std=gnu11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wno-unused-parameter",
    "-Wno-sign-conversion",
    "-Wno-shorten-64-to-32",
    "-Wno-implicit-int-conversion",
    "-I",
    "{tree}/drivers/TegraDFLL/shim/include",
    "-I",
    "{tree}/drivers/TegraDFLL/linux/include",
    "-include",
    "{tree}/drivers/TegraDFLL/shim/include/tdfll_prelude.h",
]
