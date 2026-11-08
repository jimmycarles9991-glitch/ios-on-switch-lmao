"""iOS and macOS user-space C/C++/Objective-C toolchains (https://bazel.build/tutorials/ccp-toolchain-config) on Xcode's
Apple clang, the build's one host input: the target and the SDK, and nothing else the toolchain adds. Every other
flag is the target's own (copts): NVK's objects take exactly meson's flags (third_party/mesa), so they come out as
meson made them.

  apple_target      (default) -target <triple> -isysroot <SDK> on every compile and link
  user_compile_flags, dependency_file (-MD -MF, which Bazel's .d pruning reads), compiler_input_flags (-c),
  compiler_output_flags (-o), archiver_flags (ar rcs, members in the target's source order)

No legacy features: no -iquote/-isystem for the execution root, no -fPIC, no -g/-O from the compilation mode, no
-D_FORTIFY_SOURCE, no -frandom-seed. Every tool runs with a fixed environment (ZERO_AR_DATE=1 makes archives and the
linker's debug map timestamp-free). Links are the rules' own (they call the dynamic-library link's tool with their own
arguments: third_party/mesa/bazel/link.bzl).
"""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load(
    "@rules_cc//cc:cc_toolchain_config_lib.bzl",
    "action_config",
    "env_entry",
    "env_set",
    "feature",
    "flag_group",
    "flag_set",
    "tool",
    "variable_with_value",
)
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl", "CcToolchainConfigInfo")
load("@xcode//:defs.bzl", "DEVELOPER_DIR", "IPHONEOS_SDK", "MACOSX_SDK", "XCODE_TOOLCHAIN")

# The triples: iOS 17 (iPadOS 17.7.11) and, for host twins of iOS code, this Xcode's macOS SDK's default deployment
# target (what Xcode's clang picks with no -target, as meson's native builds ran it).
PLATFORMS = {
    "ios": struct(triple = "arm64-apple-ios17.0", sdk = IPHONEOS_SDK),
    "macos": struct(triple = "arm64-apple-macosx27.2", sdk = MACOSX_SDK),
}

_C = [ACTION_NAMES.c_compile]
_CXX = [ACTION_NAMES.cpp_compile]
_OBJC = [ACTION_NAMES.objc_compile, ACTION_NAMES.objcpp_compile]
_ASM = [ACTION_NAMES.assemble, ACTION_NAMES.preprocess_assemble]
_COMPILE = _C + _CXX + _OBJC + _ASM
_LINK = [ACTION_NAMES.cpp_link_dynamic_library, ACTION_NAMES.cpp_link_executable]
_ARCHIVE = [ACTION_NAMES.cpp_link_static_library]

def _impl(ctx):
    platform = PLATFORMS[ctx.attr.platform]
    env = env_set(
        actions = _COMPILE + _LINK + _ARCHIVE,
        env_entries = [
            env_entry("PATH", "/usr/bin:/bin:/usr/sbin:/sbin"),
            env_entry("DEVELOPER_DIR", DEVELOPER_DIR),
            env_entry("LC_ALL", "C"),
            env_entry("LANG", "C"),
            env_entry("TZ", "UTC"),
            env_entry("ZERO_AR_DATE", "1"),
        ],
    )
    features = [
        feature(name = "no_legacy_features", enabled = True),
        feature(name = "apple_env", enabled = True, env_sets = [env]),
        feature(
            name = "apple_target",
            enabled = True,
            flag_sets = [flag_set(
                actions = _COMPILE + _LINK,
                flag_groups = [flag_group(flags = ["-target", platform.triple, "-isysroot", platform.sdk])],
            )],
        ),
        feature(
            name = "user_compile_flags",
            enabled = True,
            flag_sets = [flag_set(actions = _COMPILE, flag_groups = [flag_group(
                flags = ["%{user_compile_flags}"],
                iterate_over = "user_compile_flags",
                expand_if_available = "user_compile_flags",
            )])],
        ),
        feature(
            name = "dependency_file",
            enabled = True,
            flag_sets = [flag_set(actions = _COMPILE, flag_groups = [flag_group(
                flags = ["-MD", "-MF", "%{dependency_file}"],
                expand_if_available = "dependency_file",
            )])],
        ),
        feature(
            name = "compiler_input_flags",
            enabled = True,
            flag_sets = [flag_set(actions = _COMPILE, flag_groups = [flag_group(
                flags = ["-c", "%{source_file}"],
                expand_if_available = "source_file",
            )])],
        ),
        feature(
            name = "compiler_output_flags",
            enabled = True,
            flag_sets = [flag_set(actions = _COMPILE, flag_groups = [flag_group(
                flags = ["-o", "%{output_file}"],
                expand_if_available = "output_file",
            )])],
        ),
        feature(
            name = "archiver_flags",
            enabled = True,
            flag_sets = [flag_set(actions = _ARCHIVE, flag_groups = [
                flag_group(flags = ["rcs", "%{output_execpath}"], expand_if_available = "output_execpath"),
                flag_group(
                    iterate_over = "libraries_to_link",
                    expand_if_available = "libraries_to_link",
                    flag_groups = [
                        flag_group(
                            flags = ["%{libraries_to_link.name}"],
                            expand_if_equal = variable_with_value(name = "libraries_to_link.type", value = "object_file"),
                        ),
                        flag_group(
                            flags = ["%{libraries_to_link.object_files}"],
                            iterate_over = "libraries_to_link.object_files",
                            expand_if_equal = variable_with_value(name = "libraries_to_link.type", value = "object_file_group"),
                        ),
                    ],
                ),
            ])],
        ),
    ]

    if ctx.attr.link_flags:
        features.append(feature(
            name = "platform_link_flags",
            enabled = True,
            flag_sets = [flag_set(actions = _LINK, flag_groups = [flag_group(flags = ctx.attr.link_flags)])],
        ))

    clang = tool(path = XCODE_TOOLCHAIN + "/clang")
    clangxx = tool(path = XCODE_TOOLCHAIN + "/clang++")
    action_configs = [
        action_config(action_name = name, enabled = True, tools = [clang])
        for name in _C + _OBJC + _ASM
    ] + [
        action_config(action_name = name, enabled = True, tools = [clangxx])
        for name in _CXX + _LINK
    ] + [
        action_config(action_name = name, enabled = True, tools = [tool(path = XCODE_TOOLCHAIN + "/ar")])
        for name in _ARCHIVE
    ]
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "switch-apple-" + ctx.attr.platform,
        host_system_name = "aarch64-apple-darwin",
        target_system_name = platform.triple,
        target_cpu = "arm64",
        target_libc = "none",
        compiler = "apple-clang",
        abi_version = "none",
        abi_libc_version = "none",
        features = features,
        action_configs = action_configs,
        # Xcode (clang's resource headers, the SDKs): absolute, outside the execution root, declared by @xcode//:identity.
        cxx_builtin_include_directories = [DEVELOPER_DIR],
    )

apple_cc_toolchain_config = rule(
    implementation = _impl,
    doc = "A user-space CcToolchainConfigInfo on Xcode's Apple clang: the target and SDK, no other flags of its own.",
    attrs = {
        "platform": attr.string(mandatory = True, values = PLATFORMS.keys()),
        "link_flags": attr.string_list(doc = "added to every link: the platform's own (//translate/arm64e:target.bzl)"),
    },
    provides = [CcToolchainConfigInfo],
)
