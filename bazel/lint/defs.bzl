"""lint_files(): the files of one package that //bazel/lint checks, chained through its subpackages.

Every BUILD file calls it once:

    load("//bazel/lint:defs.bzl", "lint_files")

    lint_files()                                     # or lint_files(exclude = ["vendored/**"])

It declares `:lint_files`, a filegroup of the package's own lintable files (by name and extension, see PATTERNS) plus
the `:lint_files` of each direct subpackage, so `//:lint_files` is every lintable file in the repository and a
package that forgets the call breaks the chain loudly ("no such target '//x:lint_files'"). Bazel can only see files
that a package declares; this is how BUILD, .bzl, Markdown, JSON and TOML files, which no rule reads, become inputs
of cached checks. Third-party and generated files go in `exclude`.
"""

load(":clang_tidy.bzl", "RULE_KINDS", "clang_tidy_test")

PATTERNS = [
    # Starlark
    "BUILD",
    "BUILD.bazel",
    "MODULE.bazel",
    "REPO.bazel",
    "**/*.bzl",
    # Python
    "**/*.py",
    # C, C++, Objective-C, Objective-C++
    "**/*.c",
    "**/*.cc",
    "**/*.cpp",
    "**/*.h",
    "**/*.hpp",
    "**/*.inc",
    "**/*.m",
    "**/*.mm",
    # Swift
    "**/*.swift",
    # Rust, shell, Markdown, JSON, TOML, YAML
    "**/*.rs",
    "**/*.sh",
    "**/*.md",
    "**/*.json",
    "**/*.toml",
    "**/*.yml",
    "**/*.yaml",
    # Tool configuration the checks read for the files below it (clang-format's and clang-tidy's nearest-file lookup)
    "**/.clang-format",
    "**/.clang-tidy",
]

# Never ours to check, wherever they turn up in a working copy: editor and tool state, build output, the clangd
# database `bazel run //tools/lint:compdb` writes.
ALWAYS_EXCLUDE = [
    "bazel-*/**",
    "**/__pycache__/**",
    "**/.ruff_cache/**",
    "**/compile_commands.json",
    "**/*.orig",
    "**/*.rej",
]

def lint_files(
        name = "lint_files",
        exclude = [],
        dirs = None,
        subpackages = None,
        clang_tidy_clean = False,
        visibility = ["//visibility:public"]):
    """Declares the package's lintable files and those of its subpackages as one filegroup.

    By default the whole package is globbed and its subpackages are discovered (native.subpackages), both of which
    walk every directory below the package. A package whose directory also holds big trees that are not its own
    (the root: out/'s build output and scratch, which are untracked) names what to look at instead, with `dirs` and
    `subpackages`, so loading it reads a few directories rather than walking the working copy.

    Args:
        name: the filegroup's name; the chain expects the default.
        exclude: glob patterns (relative to the package) of third-party or generated files to leave alone.
        dirs: when set, only the package's top-level files and these directories (recursively) are globbed.
        subpackages: when set, the subpackages to chain (paths relative to the package), instead of discovering them.
        clang_tidy_clean: the package's :lint_files_clang_tidy test (clang-tidy over its host cc targets) is part of
            `bazel test //...`; until then it is manual.
        visibility: the filegroup's visibility; its parent package must see it.
    """
    package = native.package_name()
    if dirs == None:
        patterns = PATTERNS
    else:
        top = [p.removeprefix("**/") for p in PATTERNS]
        patterns = top + [d + "/" + p for d in dirs for p in PATTERNS]
    if subpackages == None:
        # Bazel's convenience symlinks (bazel-bin, bazel-<workspace>, ...) lead back into the tree: never through them.
        subpackages = native.subpackages(include = ["**"], exclude = ["bazel-*/**"], allow_empty = True)
    native.filegroup(
        name = name,
        srcs = native.glob(patterns, exclude = exclude + ALWAYS_EXCLUDE, allow_empty = True) + [
            "//{}:{}".format(package + "/" + sub if package else sub, name)
            for sub in subpackages
        ],
        visibility = visibility,
    )
    _clang_tidy_targets(name = name + "_clang_tidy", clean = clang_tidy_clean)

# Targets built for the device (the kernel, iOS userland) are checked through //:clang_tidy_test, which reaches them
# through //:boot and //:gpu in the configuration they build in; in the host configuration they are incompatible.
_DEVICE_CONSTRAINTS = ["bazel/platforms:kernel", "bazel/platforms:user", "os:ios"]

def _clang_tidy_targets_impl(name, visibility, clean):
    targets = []
    for rule in native.existing_rules().values():
        if rule["kind"] not in RULE_KINDS or "no-lint" in rule.get("tags", []):
            continue
        constraints = [str(c).rpartition("//")[2] for c in rule.get("target_compatible_with", [])]
        if [c for c in constraints if c in _DEVICE_CONSTRAINTS]:
            continue
        targets.append(":" + rule["name"])
    if targets:
        clang_tidy_test(
            name = name,
            targets = targets,
            # A package joins `bazel test //...` once its findings are fixed (lint_files(clang_tidy_clean = True)).
            tags = [] if clean else ["manual"],
            visibility = visibility,
        )

# A finalizer: it runs after the rest of the BUILD file, so it sees every cc target the package declares wherever the
# lint_files() call sits, and gives the package one :lint_files_clang_tidy test over its host cc targets (host tests and
# the code they compile, through their deps).
_clang_tidy_targets = macro(
    implementation = _clang_tidy_targets_impl,
    attrs = {"clean": attr.bool(configurable = False, doc = "the test is part of `bazel test //...`")},
    finalizer = True,
)
