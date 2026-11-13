"""Format and lint checks over lint_files() groups, and the formatter that fixes what they check.

    lint_test(name, srcs, checks)    a test: every file of srcs a check applies to passes it. One action per file and
                                     check, so an unchanged file is never checked twice (and the disk cache shares
                                     results between checkouts).
    format_runner(name, srcs)        `bazel run`: formats srcs in the working copy (or only the paths given as
                                     arguments), with the same tools, versions and configs the checks use.

The tools (bazel/lint/tools.bzl, @llvm, the Rust toolchain) and their configs (repository root) are declared inputs:
nothing comes from the host or from a user's own config files. swift-format is the one exception: Xcode's own
(the pinned host input, bazel/host/xcode.bzl), as rules_swift's compiler is.
"""

load("@xcode//:defs.bzl", "XCODE_TOOLCHAIN")

C_FAMILY = ["c", "cc", "cpp", "h", "hpp", "inc", "m", "mm"]
STARLARK_NAMES = ["BUILD", "BUILD.bazel", "MODULE.bazel", "REPO.bazel"]

def _is_starlark(f):
    return f.basename in STARLARK_NAMES or f.extension == "bzl"

def _is_c(f):
    return f.extension in C_FAMILY

def _is_py(f):
    return f.extension == "py"

def _is_sh(f):
    return f.extension == "sh"

def _is_rs(f):
    return f.extension == "rs"

def _is_md(f):
    return f.extension == "md"

def _is_swift(f):
    return f.extension == "swift"

def _is_github_yaml(f):
    return f.extension in ("yml", "yaml") and f.short_path.startswith(".github/")

def _is_data(f):
    return f.extension in ("json", "toml") or _is_github_yaml(f)

def _is_workflow(f):
    return _is_github_yaml(f) and f.short_path.startswith(".github/workflows/")

# Shell house style (shfmt): 4-space indent, case bodies indented, binary operators may start a line. Giving shfmt
# its style flags also makes it ignore any .editorconfig it would otherwise pick up.
SHFMT_STYLE = ["-i", "4", "-ci", "-bn"]

def _clang_format_configs(f, configs):
    """The .clang-format files clang-format can reach from f: every one in a directory above it."""
    reachable = []
    for c in configs:
        directory = c.path.rpartition("/")[0]  # "" for the repository root's
        if not directory or f.path.startswith(directory + "/"):
            reachable.append(c)
    return reachable

def _tools(ctx):
    rustfmt = ctx.attr._rustfmt[platform_common.ToolchainInfo]
    return struct(
        clang_format = ctx.executable._clang_format,
        ruff = ctx.executable._ruff,
        buildifier = ctx.executable._buildifier,
        shfmt = ctx.executable._shfmt,
        shellcheck = ctx.executable._shellcheck,
        rumdl = ctx.executable._rumdl,
        dprint = ctx.executable._dprint,
        dprint_plugins = ctx.files._dprint_plugins,
        actionlint = ctx.executable._actionlint,
        rustfmt = rustfmt.rustfmt,
        rustfmt_files = rustfmt.all_files,
        ruff_config = ctx.file._ruff_config,
        buildifier_config = ctx.file._buildifier_config,
        buildifier_tables = ctx.file._buildifier_tables,
        rumdl_config = ctx.file._rumdl_config,
        dprint_config = ctx.file._dprint_config,
        rustfmt_config = ctx.file._rustfmt_config,
        shellcheck_config = ctx.file._shellcheck_config,
        swift_format = XCODE_TOOLCHAIN + "/swift-format",
        swift_format_config = ctx.file._swift_format_config,
        xcode = ctx.file._xcode,
    )

# Each check: which files it applies to, and for one file the command (argv) and the extra inputs it reads.
def _check_clang_format(t, f, ctx_files):
    argv = [t.clang_format.path, "--style=file", "--fallback-style=none", "--dry-run", "-Werror", f.path]
    return argv, [t.clang_format] + _clang_format_configs(f, ctx_files.clang_format_configs)

def _check_ruff_format(t, f, _):
    argv = [t.ruff.path, "format", "--check", "--diff", "--no-cache", "--config", t.ruff_config.path, f.path]
    return argv, [t.ruff, t.ruff_config]

def _check_ruff(t, f, _):
    argv = [t.ruff.path, "check", "--no-cache", "--output-format=concise", "--config", t.ruff_config.path, f.path]
    return argv, [t.ruff, t.ruff_config]

# buildifier never reorders a list here (.buildifier-tables.json, which .buildifier.json adds): list order is meaning in
# this repository (a kernel_driver's srcs are its link order, an archive's members its member order, include paths
# their search order). The format check and the formatter run with -lint=off: formatting only, never a lint fix.
def _check_buildifier_format(t, f, _):
    argv = [t.buildifier.path, "-mode=check", "-lint=off", "-config=" + t.buildifier_config.path, f.path]
    return argv, [t.buildifier, t.buildifier_config, t.buildifier_tables]

def _check_buildifier(t, f, _):
    argv = [t.buildifier.path, "-mode=check", "-lint=warn", "-config=" + t.buildifier_config.path, f.path]
    return argv, [t.buildifier, t.buildifier_config, t.buildifier_tables]

def _check_shfmt(t, f, _):
    return [t.shfmt.path, "-d"] + SHFMT_STYLE + [f.path], [t.shfmt]

def _check_shellcheck(t, f, _):
    # The repository's .shellcheckrc only, never a ~/.shellcheckrc.
    argv = [t.shellcheck.path, "--rcfile", t.shellcheck_config.path, "--format=gcc", f.path]
    return argv, [t.shellcheck, t.shellcheck_config]

def _check_rustfmt(t, f, ctx_files):
    # Every .rs of the set is an input: rustfmt follows `mod` declarations to their files.
    argv = [t.rustfmt.path, "--check", "--config-path", t.rustfmt_config.path, f.path]
    return argv, [t.rustfmt, t.rustfmt_config] + ctx_files.rust + t.rustfmt_files.to_list()

def _check_rumdl(t, f, _):
    # --quiet: no summary line, which holds the run's duration.
    argv = [t.rumdl.path, "check", "--no-cache", "--quiet", "--color", "never", "--config", t.rumdl_config.path, f.path]
    return argv, [t.rumdl, t.rumdl_config]

def _check_rumdl_format(t, f, _):
    argv = [t.rumdl.path, "fmt", "--check", "--quiet", "--color", "never", "--config", t.rumdl_config.path, f.path]
    return argv, [t.rumdl, t.rumdl_config]

# actionlint checks the local actions and reusable workflows a workflow uses only inside a Git work tree, so the check
# gives the sandbox an empty .git; it runs the pinned shellcheck on the run: scripts.
def _check_actionlint(t, f, ctx_files):
    argv = ["/bin/sh", "-c", 'mkdir -p .git && exec "$0" "$@"', t.actionlint.path, "-no-color", "-pyflakes="]
    argv.append("-shellcheck=" + t.shellcheck.path)
    argv.extend(["-config-file=" + c.path for c in ctx_files.github_yaml if c.short_path == ".github/actionlint.yaml"])
    return argv + [f.path], [t.actionlint, t.shellcheck] + ctx_files.github_yaml

def _check_dprint(t, f, _):
    # dprint takes the plugins after the files.
    argv = [t.dprint.path, "check", "--config", t.dprint_config.path, f.path, "--plugins"]
    argv.extend([p.path for p in t.dprint_plugins])
    return argv, [t.dprint, t.dprint_config] + t.dprint_plugins

# swift-format's own format check is `lint`, which also reports its style rules; formatting alone is the diff between
# the file and what `format` makes of it. It skips symbolic links (every sandboxed input is one) unless told not to.
def _check_swift_format(t, f, _):
    command = 'out="$(mktemp)" && "$0" format --follow-symlinks --configuration "$1" "$2" > "$out" && diff -u "$2" "$out"'
    return ["/bin/sh", "-c", command, t.swift_format, t.swift_format_config.path, f.path], [t.swift_format_config, t.xcode]

def _check_swift_lint(t, f, _):
    argv = [t.swift_format, "lint", "--strict", "--follow-symlinks", "--configuration", t.swift_format_config.path, f.path]
    return argv, [t.swift_format_config, t.xcode]

CHECKS = {
    "actionlint": (_is_workflow, _check_actionlint),
    "buildifier": (_is_starlark, _check_buildifier),
    "buildifier_format": (_is_starlark, _check_buildifier_format),
    "clang_format": (_is_c, _check_clang_format),
    "dprint": (_is_data, _check_dprint),
    "ruff": (_is_py, _check_ruff),
    "ruff_format": (_is_py, _check_ruff_format),
    "rumdl": (_is_md, _check_rumdl),
    "rumdl_format": (_is_md, _check_rumdl_format),
    "rustfmt": (_is_rs, _check_rustfmt),
    "shellcheck": (_is_sh, _check_shellcheck),
    "shfmt": (_is_sh, _check_shfmt),
    "swift_format": (_is_swift, _check_swift_format),
    "swift_lint": (_is_swift, _check_swift_lint),
}

# Runs the check; its report is empty when it passes and holds the tool's output when it fails. The action itself
# always succeeds, so `bazel build //...` never stops on a finding and the test lists every finding at once. sed makes
# the paths relative to the workspace, not the sandbox (bash 3.2's ${out//pattern/} is quadratic in the output).
_RUN = """\
report="$1"; shift
export DPRINT_CACHE_DIR="$(mktemp -d)" RUMDL_CACHE_DIR="$(mktemp -d)" NO_COLOR=1
log="$(mktemp)"
trap 'rm -rf "$DPRINT_CACHE_DIR" "$RUMDL_CACHE_DIR" "$log"' EXIT
if "$@" > "$log" 2>&1; then
    : > "$report"
else
    rc=$?
    sed "s|$PWD/||g" "$log" > "$report"
    [ -s "$report" ] || echo "(no output)" > "$report"
    echo "exit $rc: $*" >> "$report"
fi
"""

_TEST = """\
#!/bin/bash
# {name}: {count} checks ({checks}). A file the exempt list names may fail its checks, and must: one that passes
# fails the test until it leaves the list, so the list only shrinks.
failed=0
exempted=0
exempt=$'\\n'"$(grep -v '^#' {exempt} 2> /dev/null)"$'\\n'
while IFS=$'\\t' read -r report file; do
    case "$exempt" in
    *$'\\n'"$file"$'\\n'*)
        if [ -s "$report" ]; then
            exempted=$((exempted + 1))
        else
            echo "$file is formatted now: remove it from {exempt}"
            failed=$((failed + 1))
        fi
        continue
        ;;
    esac
    if [ -s "$report" ]; then
        cat "$report"
        echo
        failed=$((failed + 1))
    fi
done < {manifest}
[ "$exempted" -eq 0 ] || echo "{name}: $exempted checks of files {exempt} exempts failed, as it expects."
if [ "$failed" -ne 0 ]; then
    echo "{name}: $failed of {count} checks failed. \\`bazel run //:format\\` fixes formatting."
    exit 1
fi
echo "{name}: {count} checks passed."
"""

def _lint_test_impl(ctx):
    t = _tools(ctx)
    files = [f for f in ctx.files.srcs if f.is_source]
    ctx_files = struct(
        clang_format_configs = [f for f in files if f.basename == ".clang-format"],
        rust = [f for f in files if _is_rs(f)],
        github_yaml = [f for f in files if _is_github_yaml(f)],
    )
    reports = []
    for check in ctx.attr.checks:
        if check not in CHECKS:
            fail("unknown check {} (known: {})".format(check, ", ".join(sorted(CHECKS))))
        applies, command = CHECKS[check]
        for f in files:
            if not applies(f):
                continue
            argv, inputs = command(t, f, ctx_files)
            report = ctx.actions.declare_file("{}/{}/{}.report".format(ctx.label.name, check, f.short_path))
            ctx.actions.run_shell(
                command = _RUN,
                arguments = [report.path] + argv,
                inputs = depset([f] + inputs),
                outputs = [report],
                mnemonic = "Lint" + "".join([p.capitalize() for p in check.split("_")]),
                progress_message = "{} %{{label}}: {}".format(check, f.short_path),
            )
            reports.append((report, f))
    manifest = ctx.actions.declare_file(ctx.label.name + ".reports")
    ctx.actions.write(manifest, "".join([r.short_path + "\t" + f.short_path + "\n" for r, f in reports]))
    script = ctx.actions.declare_file(ctx.label.name + ".sh")
    exempt = ctx.file.exempt
    ctx.actions.write(script, _TEST.format(
        name = ctx.label.name,
        count = len(reports),
        checks = ", ".join(ctx.attr.checks),
        manifest = manifest.short_path,
        exempt = exempt.short_path if exempt else "/dev/null",
    ), is_executable = True)
    runfiles = [manifest] + [r for r, _ in reports] + ([exempt] if exempt else [])
    return [DefaultInfo(executable = script, runfiles = ctx.runfiles(files = runfiles))]

_TOOL_ATTRS = {
    "_actionlint": attr.label(default = "@actionlint//:actionlint", executable = True, cfg = "exec", allow_single_file = True),
    "_buildifier": attr.label(default = "@buildifier//file", executable = True, cfg = "exec", allow_single_file = True),
    "_buildifier_config": attr.label(default = "//:.buildifier.json", allow_single_file = True),
    "_buildifier_tables": attr.label(default = "//:.buildifier-tables.json", allow_single_file = True),
    "_clang_format": attr.label(default = "@llvm//:bin/clang-format", executable = True, cfg = "exec", allow_single_file = True),
    "_dprint": attr.label(default = "@dprint//:dprint", executable = True, cfg = "exec", allow_single_file = True),
    "_dprint_config": attr.label(default = "//:dprint.json", allow_single_file = True),
    "_dprint_plugins": attr.label_list(default = ["@dprint_plugin_json//file", "@dprint_plugin_toml//file", "@dprint_plugin_yaml//file"], allow_files = True),
    "_ruff": attr.label(default = "@ruff//:ruff", executable = True, cfg = "exec", allow_single_file = True),
    "_ruff_config": attr.label(default = "//:ruff.toml", allow_single_file = True),
    "_rumdl": attr.label(default = "@rumdl//:rumdl", executable = True, cfg = "exec", allow_single_file = True),
    "_rumdl_config": attr.label(default = "//:.rumdl.toml", allow_single_file = True),
    "_rustfmt": attr.label(default = "@rules_rust//rust/toolchain:current_rustfmt_toolchain", cfg = "exec"),
    "_rustfmt_config": attr.label(default = "//:rustfmt.toml", allow_single_file = True),
    "_shellcheck": attr.label(default = "@shellcheck//:shellcheck", executable = True, cfg = "exec", allow_single_file = True),
    "_shellcheck_config": attr.label(default = "//:.shellcheckrc", allow_single_file = True),
    "_shfmt": attr.label(default = "@shfmt//file", executable = True, cfg = "exec", allow_single_file = True),
    "_swift_format_config": attr.label(default = "//:.swift-format", allow_single_file = True),
    "_xcode": attr.label(default = "@xcode//:identity", allow_single_file = True),
}

lint_test = rule(
    implementation = _lint_test_impl,
    doc = "Checks every file of srcs (lint_files() groups) that the named checks apply to; see CHECKS.",
    attrs = dict(_TOOL_ATTRS, **{
        "checks": attr.string_list(mandatory = True, doc = "names from CHECKS"),
        "exempt": attr.label(allow_single_file = True, doc = "workspace paths, one a line, whose checks must fail"),
        "srcs": attr.label_list(allow_files = True, mandatory = True, doc = "lint_files() groups"),
    }),
    test = True,
)

# The formatters in fix mode, each over its files (relative to the workspace root, the working directory).
_FORMAT = """\
#!/bin/bash
# Formats the files `bazel run {label}` was built with, in place. With arguments, only those files and the files
# under those directories. (/bin/bash is 3.2 on macOS: no set -u with empty arrays, no namerefs, no mapfile.)
set -eo pipefail
runfiles="$PWD"
CLANG_FORMAT="$runfiles/{clang_format}"
RUFF="$runfiles/{ruff}"
BUILDIFIER="$runfiles/{buildifier}"
SHFMT="$runfiles/{shfmt}"
RUSTFMT="$runfiles/{rustfmt}"
RUMDL="$runfiles/{rumdl}"
DPRINT="$runfiles/{dprint}"
DPRINT_PLUGINS=({dprint_plugins})
SWIFT_FORMAT="{swift_format}"
export DPRINT_CACHE_DIR="$(mktemp -d)" RUMDL_CACHE_DIR="$(mktemp -d)" NO_COLOR=1
trap 'rm -rf "$DPRINT_CACHE_DIR" "$RUMDL_CACHE_DIR"' EXIT
cd "$BUILD_WORKSPACE_DIRECTORY"
ARGS=("$@")

pick() {{
    # The files of the named list, limited to the command line's paths when there are any (bash 3.2: no namerefs).
    local f a
    eval 'set -- "${{'"$1"'[@]}}"'
    for f in "$@"; do
        if [ "${{#ARGS[@]}}" -eq 0 ]; then echo "$f"; continue; fi
        for a in "${{ARGS[@]}}"; do
            a="${{a%/}}"
            if [ "$f" = "$a" ] || [ "${{f#"$a"/}}" != "$f" ]; then echo "$f"; break; fi
        done
    done
}}

run() {{
    # run NAME LIST COMMAND...: COMMAND FILE... over the picked files of LIST, four batches at a time (a shared machine).
    local name="$1" list="$2" files
    shift 2
    files="$(pick "$list")"
    [ -n "$files" ] || return 0
    printf '%s\\n' "$files" | tr '\\n' '\\0' | xargs -0 -n 64 -P 4 "$@"
    echo "{label}: $name: $(printf '%s\\n' "$files" | wc -l | tr -d ' ') files"
}}

C_FILES=({c_files})
PY_FILES=({py_files})
STARLARK_FILES=({starlark_files})
SH_FILES=({sh_files})
RS_FILES=({rs_files})
MD_FILES=({md_files})
DATA_FILES=({data_files})
SWIFT_FILES=({swift_files})

run clang-format C_FILES "$CLANG_FORMAT" -i --style=file --fallback-style=none
run ruff PY_FILES "$RUFF" format --quiet --no-cache --config ruff.toml
run buildifier STARLARK_FILES "$BUILDIFIER" -mode=fix -lint=off -config=.buildifier.json
run shfmt SH_FILES "$SHFMT" -w {shfmt_style}
run rustfmt RS_FILES "$RUSTFMT" --config-path rustfmt.toml
run rumdl MD_FILES "$RUMDL" fmt --silent --color never --config .rumdl.toml
run swift-format SWIFT_FILES "$SWIFT_FORMAT" format --in-place --configuration .swift-format
# dprint takes the plugins after the files.
run dprint DATA_FILES bash -c 'exec "$0" fmt --config dprint.json "$@" --plugins '"${{DPRINT_PLUGINS[*]}}" "$DPRINT"
"""

def _quote(paths):
    return " ".join(["'{}'".format(p) for p in paths])

def _format_runner_impl(ctx):
    t = _tools(ctx)
    files = [f for f in ctx.files.srcs if f.is_source]
    script = ctx.actions.declare_file(ctx.label.name + ".sh")
    rlocation = lambda f: f.short_path
    ctx.actions.write(script, _FORMAT.format(
        label = str(ctx.label),
        clang_format = rlocation(t.clang_format),
        ruff = rlocation(t.ruff),
        buildifier = rlocation(t.buildifier),
        shfmt = rlocation(t.shfmt),
        rustfmt = rlocation(t.rustfmt),
        rumdl = rlocation(t.rumdl),
        dprint = rlocation(t.dprint),
        dprint_plugins = " ".join(['"$runfiles/{}"'.format(rlocation(p)) for p in t.dprint_plugins]),
        shfmt_style = " ".join(SHFMT_STYLE),
        swift_format = t.swift_format,
        c_files = _quote([f.short_path for f in files if _is_c(f)]),
        py_files = _quote([f.short_path for f in files if _is_py(f)]),
        starlark_files = _quote([f.short_path for f in files if _is_starlark(f)]),
        sh_files = _quote([f.short_path for f in files if _is_sh(f)]),
        rs_files = _quote([f.short_path for f in files if _is_rs(f)]),
        md_files = _quote([f.short_path for f in files if _is_md(f)]),
        data_files = _quote([f.short_path for f in files if _is_data(f)]),
        swift_files = _quote([f.short_path for f in files if _is_swift(f)]),
    ), is_executable = True)
    tools = [t.clang_format, t.ruff, t.buildifier, t.shfmt, t.rustfmt, t.rumdl, t.dprint] + t.dprint_plugins
    return [DefaultInfo(
        executable = script,
        runfiles = ctx.runfiles(files = tools, transitive_files = t.rustfmt_files),
    )]

format_runner = rule(
    implementation = _format_runner_impl,
    doc = "`bazel run` target that formats srcs (lint_files() groups) in place.",
    attrs = dict(_TOOL_ATTRS, **{
        "srcs": attr.label_list(allow_files = True, mandatory = True, doc = "lint_files() groups"),
    }),
    executable = True,
)
