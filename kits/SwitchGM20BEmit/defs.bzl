"""Mesa's generated push helpers for GM20B's classes, and what a kernel driver adds to use the kit.

Each class's nv_push_<cl>.h is Mesa's class_parser.py output (mesa_gen/, unchanged, for Mesa's own sources built on
the host) and that output made kernel-safe (gen/, mesa/kernel_safe_push_header.py) for everything else."""

load("//kits/SwitchGM20B:defs.bzl", "SWITCH_GM20B_INCLUDES")

_MESA_HEADERS = "//third_party/mesa/src/nouveau/headers"

# The kit's sources, for user space (:emit) and the kernel's compile check alike.
EMIT_SOURCES = [
    "SwitchGM20BEmit2D.c",
    "SwitchGM20BEmit3D.c",
    "SwitchGM20BEmitBarrier.c",
    "SwitchGM20BEmitDecompress.c",
    "SwitchGM20BEmitIterated.c",
    "SwitchGM20BEmitPredicate.c",
    "SwitchGM20BEmitRaster.c",
    "SwitchGM20BEmitReport.c",
    "mesa/nouveau_copy_rect.c",
]

# GM20B's engine classes, and the Fermi and Turing classes Mesa's MME builder and simulator name.
PUSH_CLASSES = ["clb197", "clb1c0", "clb0b5", "cl902d", "cla140", "clb06f", "cl906f", "cl9097", "clc597"]

# Mesa's MME builder and simulator, built for the host (Mesa's own targets build only in NVK's configuration), and the
# headers they include.
MESA_MME = "//third_party/mesa/src/nouveau/mme:"
MESA_MME_SOURCES = [MESA_MME + f for f in ["mme_fermi.c", "mme_fermi_builder.c", "mme_tu104.c", "mme_tu104_builder.c"]]
MESA_MME_HEADERS = [MESA_MME + f + ".h" for f in [
    "mme_bitpack_helpers",
    "mme_builder",
    "mme_fermi",
    "mme_fermi_builder",
    "mme_fermi_sim",
    "mme_sim",
    "mme_tu104",
    "mme_tu104_builder",
    "mme_value",
]] + ["//third_party/mesa/src/util:" + f + ".h" for f in [
    "bitpack_helpers",
    "bitscan",
    "detect_arch",
    "detect_cc",
    "enum_operators",
    "macros",
    "u_endian",
    "u_math",
]] + [
    "//third_party/mesa:include/c99_compat.h",
    "//third_party/mesa:include/no_extern_c.h",
    "//third_party/mesa/src/nouveau/headers:nv_device_info.h",
    "//third_party/mesa/src/nouveau/headers:nvidia/classes/cl9097.h",
    "//third_party/mesa/src/nouveau/headers:nvidia/classes/clc597.h",
    "//third_party/mesa/src/nouveau/headers:nvidia/classes/clcd97.h",
]
MESA_MME_COPTS = [
    "-Ithird_party/mesa/src",
    "-Ithird_party/mesa/include",
    "-Ithird_party/mesa/src/nouveau/headers",
    "-Ithird_party/mesa/src/nouveau/headers/nvidia/classes",
    "-Ithird_party/mesa/src/nouveau/mme",
    "-I$(GENDIR)/kits/SwitchGM20BEmit/mesa_gen",
    "-DHAVE_FUNC_ATTRIBUTE_UNUSED",
    "-Wno-unused-parameter",
    "-Wno-sign-compare",
    "-Wno-missing-field-initializers",
    "-Wno-unused-function",
]

# The kernel toolchain passes no cc_library include paths: a kernel driver names them.
SWITCH_GM20B_EMIT_INCLUDES = SWITCH_GM20B_INCLUDES + [
    "-I",
    "kits/SwitchGM20BEmit",
    "-I",
    "kits/SwitchGM20BEmit/mesa",
    "-I",
    "$(GENDIR)/kits/SwitchGM20BEmit/gen",
]

def push_headers(name, classes = PUSH_CLASSES):
    """`name`: the kernel-safe headers (gen/nv_push_<cl>.h); `name`_mesa: Mesa's own (mesa_gen/nv_push_<cl>.h)."""
    safe, pristine = [], []
    for cl in classes:
        header = "{}:nvidia/classes/{}.h".format(_MESA_HEADERS, cl)
        mesa_out = "mesa_gen/nv_push_{}.h".format(cl)
        native.genrule(
            name = "nv_push_{}_mesa".format(cl),
            srcs = [header],
            outs = [mesa_out],
            cmd = "$(execpath {}:class_parser) --in-h $(execpath {}) --out-h $@".format(_MESA_HEADERS, header),
            tools = ["{}:class_parser".format(_MESA_HEADERS)],
        )
        safe_out = "gen/nv_push_{}.h".format(cl)
        native.genrule(
            name = "nv_push_{}".format(cl),
            srcs = [mesa_out],
            outs = [safe_out],
            cmd = "$(execpath :kernel_safe_push_header) $(execpath {}) $@ {}.h".format(mesa_out, cl),
            tools = [":kernel_safe_push_header"],
        )
        safe.append(safe_out)
        pristine.append(mesa_out)
    native.filegroup(name = name, srcs = safe)
    native.filegroup(name = name + "_mesa", srcs = pristine)
