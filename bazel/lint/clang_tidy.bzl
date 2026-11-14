"""clang-tidy (LLVM 23.1.4, the root .clang-tidy) over the build's C, C++, Objective-C and Objective-C++ targets.

    clang_tidy_test(name, targets)   a test: every first-party unit reachable from targets passes clang-tidy, warnings
                                     as errors. One action per translation unit, so an unchanged unit is never checked
                                     twice (and the disk cache shares results between checkouts).

The aspect follows the build graph from targets (ATTRS: cc deps, kernel_archive members, xnu_kernel inputs, the boot
package's overlay, ...), so each unit is checked in the configuration the build compiles it in (a kernel driver in
//bazel/platforms:ios_kernel_arm64) with the flags its compile action gets: the cc_toolchain's features plus the
target's copts, defines and include paths. Two toolchain features are left out of that feature configuration, with no
transition and so no second configuration of anything: apple_clang_only (options only Apple's clang knows, or that
only matter for code generation; upstream clang-tidy rejects them) and kernel_warnings (the drivers' -W/-Werror set:
-Werror would turn any Apple-vs-upstream diagnostic difference into a failure; clang-tidy's own checks are the lint).
XNU's include directories (its sources, exported headers and SDK root, all below kernel/xnu-<version>) are passed as
-isystem instead of -I, so a finding inside one of Apple's macros (kIOReturn*'s signed shifts, IONew's sizeof * n) is
Apple's, not the unit's. The build itself keeps -I.

Units of targets tagged "no-lint" (vendored code: nouveau, Linux ports, ...) and of other repositories are skipped.
"""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain", "use_cpp_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

TidyInfo = provider(doc = "clang-tidy reports of a target and everything it reaches.", fields = ["reports"])

# The attributes the aspect follows to reach cc targets (the kernel and boot rules', and rules_cc's own).
ATTRS = [
    "cc_deps",
    "deps",
    "implementation_deps",
    "inputs",
    "kernel",
    "members",
    "overlay",
]

RULE_KINDS = ["cc_binary", "cc_library", "cc_test", "objc_library"]

UNSUPPORTED_FEATURES = ["apple_clang_only", "kernel_warnings", "layering_check", "parse_headers"]

# Extension: (compile action whose flags the unit gets, clang -x language)
LANGUAGES = {
    "c": (ACTION_NAMES.c_compile, "c"),
    "cc": (ACTION_NAMES.cpp_compile, "c++"),
    "cpp": (ACTION_NAMES.cpp_compile, "c++"),
    "cxx": (ACTION_NAMES.cpp_compile, "c++"),
    "m": (ACTION_NAMES.objc_compile, "objective-c"),
    "mm": (ACTION_NAMES.objcpp_compile, "objective-c++"),
}

# Apple clang's XNU type-system builtins (kalloc_type, IOMallocType) take a type; upstream clang has none of them. For
# analysis only, macros that drop the type stand in (they never reach a real compile).
EXTRA_ARGS = [
    "-D__builtin_xnu_type_signature(t)=\"switch-lint\"",
    "-D__builtin_xnu_type_summary(t)=0u",
    "-D__builtin_xnu_types_compatible(a,b)=1",
    "-Wno-unknown-warning-option",
    "-Wno-unused-command-line-argument",
]

# The report is empty when the unit passes and holds clang-tidy's findings when it does not; the action itself always
# succeeds, so the test lists every finding at once. The report is a function of the inputs: paths relative to the
# workspace (sed, not bash 3.2's quadratic ${out//pattern/}), each finding (its line and the notes and source lines
# under it, joined by awk) once and sorted, then the exit status.
# The Apple toolchain's flags name the SDK and Xcode by placeholders that its compiler wrapper resolves. clang-tidy has
# no wrapper, so they are resolved here from SDKROOT and DEVELOPER_DIR, which the executor sets from the action's Xcode
# environment on whichever Mac runs it.
_RUN = """\
report="$1"; unit="$2"; shift 2
for arg in "$@"; do
    arg="${arg//__BAZEL_XCODE_SDKROOT__/${SDKROOT:-}}"
    set -- "$@" "${arg//__BAZEL_XCODE_DEVELOPER_DIR__/${DEVELOPER_DIR:-}}"
    shift
done
log="$(mktemp)"
if "$@" > "$log" 2>&1; then
    : > "$report"
else
    rc=$?
    sed "s|$PWD/||g" "$log" |
        awk '/^[^ ]+:[0-9]+:[0-9]+: (warning|error): / { if (f != "") print f; f = $0; next } { f = f "\\036" $0 }
             END { if (f != "") print f }' |
        LC_ALL=C sort -u | tr '\\036' '\\n' > "$report"
    [ -s "$report" ] || echo "(no output)" > "$report"
    echo "exit $rc: clang-tidy $unit" >> "$report"
fi
rm -f "$log"
"""

def _flags(cc_toolchain, feature_configuration, compilation_context, action, user_flags):
    variables = cc_common.create_compile_variables(
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        user_compile_flags = user_flags,
        include_directories = compilation_context.includes,
        quote_include_directories = compilation_context.quote_includes,
        system_include_directories = depset(transitive = [
            compilation_context.system_includes,
            compilation_context.external_includes,
        ]),
        framework_include_directories = compilation_context.framework_includes,
        preprocessor_defines = depset(transitive = [compilation_context.defines, compilation_context.local_defines]),
    )
    return cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = action,
        variables = variables,
    )

def _apple_headers_as_system(flags):
    out = list(flags)
    for i, flag in enumerate(out):
        if flag == "-I" and i + 1 < len(out) and "kernel/xnu-" in out[i + 1]:
            out[i] = "-isystem"
        elif flag.startswith("-I") and "kernel/xnu-" in flag:
            out[i] = "-isystem" + flag[2:]
    return out

def _make_variables(ctx):
    """The make variables a target's copts use: Bazel's own ($(BINDIR), ...) and its toolchains' ($(KERNEL_XNU), ...)."""
    variables = dict(ctx.var)
    for toolchain in getattr(ctx.rule.attr, "toolchains", []):
        if platform_common.TemplateVariableInfo in toolchain:
            variables.update(toolchain[platform_common.TemplateVariableInfo].variables)
    return variables

def _expand(flags, variables):
    expanded = []
    for flag in flags:
        for name, value in variables.items():
            flag = flag.replace("$(" + name + ")", value)
        expanded.append(flag)
    return expanded

def _tidy_units(target, ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features + UNSUPPORTED_FEATURES,
    )
    compilation_context = target[CcInfo].compilation_context
    variables = _make_variables(ctx)
    copts = _expand(getattr(ctx.rule.attr, "copts", []), variables)
    configs = [f for f in ctx.files._configs if f.basename in (".clang-tidy", ".clang-format")]
    resource_dir = ctx.files._resource_dir[0].path.partition("/include/")[0]
    reports = []
    for src in ctx.rule.files.srcs:
        if not src.is_source or src.extension not in LANGUAGES:
            continue
        action, language = LANGUAGES[src.extension]
        if language in ("c", "objective-c"):
            lang_flags = ctx.fragments.cpp.conlyopts + _expand(getattr(ctx.rule.attr, "conlyopts", []), variables)
        else:
            lang_flags = ctx.fragments.cpp.cxxopts + _expand(getattr(ctx.rule.attr, "cxxopts", []), variables)
        flags = _apple_headers_as_system(_flags(
            cc_toolchain,
            feature_configuration,
            compilation_context,
            action,
            ctx.fragments.cpp.copts + copts + lang_flags,
        ))
        report = ctx.actions.declare_file("{}.clang_tidy/{}.report".format(target.label.name, src.short_path))

        # The root .clang-tidy included: without it in the sandbox, clang-tidy walks up out of the sandbox and reads
        # whatever .clang-tidy lies above the output base, or none.
        reachable = [
            c
            for c in configs
            if "/" not in c.short_path or src.short_path.startswith(c.short_path.rpartition("/")[0] + "/")
        ]
        args = ctx.actions.args()
        args.add(report)
        args.add(src.short_path)
        args.add(ctx.executable._clang_tidy)
        args.add("--quiet")
        args.add("--warnings-as-errors=*")
        args.add_all(EXTRA_ARGS, format_each = "--extra-arg=%s")
        args.add(src)
        args.add("--")
        args.add_all(flags)
        args.add("-resource-dir=" + resource_dir)
        args.add("-x", language)
        ctx.actions.run_shell(
            command = _RUN,
            arguments = [args],
            inputs = depset(
                [src] + reachable + ctx.files._resource_dir,
                transitive = [compilation_context.headers, cc_toolchain.all_files],
            ),
            tools = [ctx.executable._clang_tidy],
            outputs = [report],
            env = cc_common.get_environment_variables(
                feature_configuration = feature_configuration,
                action_name = action,
                variables = cc_common.create_compile_variables(
                    feature_configuration = feature_configuration,
                    cc_toolchain = cc_toolchain,
                ),
            ),
            mnemonic = "ClangTidy",
            progress_message = "clang-tidy %{label}: " + src.short_path,
        )
        reports.append(report)
    return reports

def _clang_tidy_aspect_impl(target, ctx):
    transitive = []
    for name in ATTRS:
        value = getattr(ctx.rule.attr, name, None)
        for dep in (value if type(value) == "list" else [value] if value else []):
            if type(dep) == "Target" and TidyInfo in dep:
                transitive.append(dep[TidyInfo].reports)
    reports = []
    if (ctx.rule.kind in RULE_KINDS and CcInfo in target and target.label.workspace_name == "" and
        "no-lint" not in ctx.rule.attr.tags):
        reports = _tidy_units(target, ctx)
    return [TidyInfo(reports = depset(reports, transitive = transitive))]

clang_tidy_aspect = aspect(
    implementation = _clang_tidy_aspect_impl,
    attr_aspects = ATTRS,
    attrs = {
        "_clang_tidy": attr.label(default = "@llvm//:bin/clang-tidy", executable = True, cfg = "exec", allow_single_file = True),
        # .clang-tidy (and .clang-format, for clang-tidy's FormatStyle) files, wherever lint_files() collects them.
        "_configs": attr.label(default = "//:lint_files"),
        "_resource_dir": attr.label(default = "@llvm//:clang_resource_headers"),
    },
    fragments = ["cpp"],
    toolchains = use_cpp_toolchain(),
)

_TEST = """\
#!/bin/bash
# {name}: clang-tidy over {count} units.
failed=0
while IFS= read -r report; do
    if [ -s "$report" ]; then
        cat "$report"
        echo
        failed=$((failed + 1))
    fi
done < {manifest}
if [ "$failed" -ne 0 ]; then
    echo "{name}: $failed of {count} units have clang-tidy findings."
    exit 1
fi
echo "{name}: {count} units passed clang-tidy."
"""

def _clang_tidy_test_impl(ctx):
    reports = depset(transitive = [t[TidyInfo].reports for t in ctx.attr.targets]).to_list()
    manifest = ctx.actions.declare_file(ctx.label.name + ".reports")
    ctx.actions.write(manifest, "".join([r.short_path + "\n" for r in reports]))
    script = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(
        script,
        _TEST.format(name = ctx.label.name, count = len(reports), manifest = manifest.short_path),
        is_executable = True,
    )
    return [DefaultInfo(executable = script, runfiles = ctx.runfiles(files = [manifest] + reports))]

clang_tidy_test = rule(
    implementation = _clang_tidy_test_impl,
    doc = "clang-tidy over every first-party C/C++/ObjC unit the targets reach; see the module docstring.",
    attrs = {
        "targets": attr.label_list(aspects = [clang_tidy_aspect], doc = "where the aspect starts: //:boot, //:gpu, ..."),
    },
    test = True,
)
