"""The shapes the raw driver's host tests share, and Objective-C++ units for host libraries."""

load("//bazel/testing:defs.bzl", "host_test")

# The headers the runtime's units and the facade's Objective-C++ units include
METAL_HOST_HEADERS = [
    "//drivers/TegraGM20BMetal:plugin_headers",
    "//drivers/TegraGM20B:TegraGM20BABI.h",
    "//drivers/TegraGM20B:TegraGM20BClockABI.h",
    "//drivers/TegraGM20B:TegraGM20BCompilerABI.h",
    "//drivers/TegraGM20B:TegraGM20BFencePageABI.h",
    "//drivers/TegraGM20B:TegraGM20BGrPreemptABI.h",
    "//drivers/TegraGM20B:TegraGM20BHost.h",
    "//drivers/TegraGM20B:TegraGM20BJobABI.h",
    "//drivers/TegraGM20B:TegraGM20BLayout.h",
    "//drivers/TegraGM20B:TegraGM20BPathsABI.h",
    "//drivers/TegraGM20B:TegraGM20BSchedABI.h",
    "//drivers/TegraGM20B:TegraGM20BSchedPolicy.h",
    "//drivers/TegraGM20B:TegraGM20BVolatileABI.h",
    "//drivers/TegraGM20B:TegraGM20BZcullABI.h",
    "//drivers/TegraGM20BMetal:compiler_headers",
    "//drivers/TegraGM20BMetal:gm20b_runtime_headers",
    "//kits/SwitchGM20B:class_headers",
    "//kits/SwitchGM20BEmit:emit_headers",
    "//kits/SwitchDMA:SwitchDcache.h",
    "//kits/SwitchRefusal:SwitchRefusal.h",
    "//kits/SwitchRefusal:SwitchRefusalSite.h",
    "//kits/SwitchSeedPack:SwitchSeedPack.h",
    "//kits/SwitchSeedPack:SwitchSeedStore.h",
]

METAL_HOST_COPTS = [
    "-Idrivers/TegraGM20BMetal/plugin",
    "-Idrivers/TegraGM20B",
    "-Idrivers/TegraGM20BMetal",
    "-Ikits/SwitchRefusal",
    "-Ikits/SwitchGM20BEmit/mesa",
    "-I$(GENDIR)/kits/SwitchGM20BEmit/gen",
    "-Ithird_party/mesa/src/nouveau/headers",
    "-I$(GENDIR)/kits/SwitchGM20B",
    "-Wno-deprecated-declarations",
    # the plugin's key (gpu/defs.bzl): any, as a host test's pack takes
    "-DTEGRA_STACK_KEY=0ull",
    "-DSWITCH_STATS_GPU=1",
    "-DSWITCH_PROF_GPU=1",
]

METAL_HOST_DEPS = [
    "//drivers/TegraGM20BMetal/abi:metal_abi",
    "//kits/SwitchGM20B:caps",
    "//kits/SwitchGM20BEmit:macros_h",
    "//third_party/mesa/src/nouveau/switch_compile:host_cc",
]

# The harness marks make device_harness.h a friend of the device and change compute.h's layout: a test's own
# units and :device_runtime's take the same ones
METAL_DEVICE_COPTS = [
    "-Idrivers/TegraGM20B",
    "-DSWITCH_METAL_HARNESS_MARKS",
    "-DSWITCH_STATS_GPU=1",
    "-DSWITCH_PROF_GPU=1",
]

FACADE_LINKOPTS = [
    "-framework",
    "Foundation",
    "-framework",
    "Metal",
    "-framework",
    "IOSurface",
    "-framework",
    "IOKit",
    "-framework",
    "CoreFoundation",
    "-lobjc",
]

def device_test(name, srcs, copts = [], linkopts = [], deps = [], **kwargs):
    """A host_test of gm20b::Device over the kernel model (device_harness.h), linked with :device_runtime.

    A runtime unit the library leaves out goes in `srcs`, where it builds with the same flags.
    """
    host_test(
        name = name,
        srcs = srcs + [
            "device_harness.h",
            "job_methods.h",
            "kernel_model.h",
        ],
        copts = METAL_DEVICE_COPTS + copts,
        cxx_std = "c++17",
        linkopts = linkopts + [
            "-framework",
            "IOKit",
            "-framework",
            "IOSurface",
            "-framework",
            "CoreFoundation",
        ],
        deps = [":device_runtime"] + deps,
        **kwargs
    )

def facade_test(name, srcs, plugin = "_host", copts = [], linkopts = [], data = [], env = {}, deps = [], **kwargs):
    """A host_test of the facade over the kernel model (facade_harness.h).

    It links :facade_plugin<plugin> and :runtime_plugin<plugin>; the translation is translate_absent.cpp's.
    """
    host_test(
        name = name,
        srcs = srcs + [
            "empty_pack.h",
            "made_metallib.h",
            "facade_harness.h",
            "job_methods.h",
            "request_store_key.h",
            "translate_absent.cpp",
            "kernel_model.h",
        ] + METAL_HOST_HEADERS,
        copts = [
            "-x",
            "objective-c++",
            "-fobjc-arc",
        ] + METAL_HOST_COPTS + copts,
        cxx_std = "c++17",
        data = data,
        env = env,
        linkopts = FACADE_LINKOPTS + linkopts,
        sanitize = False,
        deps = [
            ":facade_plugin" + plugin,
            ":runtime_plugin" + plugin,
            "//kits/SwitchRefusal:switch_refusal",
            "//kits/SwitchGM20BEmit/test:method_checks",
        ] + METAL_HOST_DEPS + deps,
        **kwargs
    )

def _objcxx_units_impl(ctx):
    units = []
    for src in ctx.files.srcs:
        unit = ctx.actions.declare_file("{}/{}.cpp".format(ctx.label.name, src.basename))
        ctx.actions.write(unit, '#include "{}"\n'.format(src.path))
        units.append(unit)
    return [DefaultInfo(files = depset(units))]

objcxx_units = rule(
    implementation = _objcxx_units_impl,
    doc = """One <basename>.cpp per Objective-C++ source, including it by its execution-root path.

cc rules take no .mm source, so each one is compiled through a .cpp of its own that includes it (with
`-x objective-c++` in the library's copts, and the .mm files among its additional_compiler_inputs).""",
    attrs = {"srcs": attr.label_list(allow_files = [".mm"], mandatory = True)},
)
