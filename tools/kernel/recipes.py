"""The kernel toolchain's compile recipes (bazel/toolchains/kernel/recipes.bzl), from the native XNU build's own tables.

A driver compiles with the flags XNU compiles one of its objects with: kernel/xnu-<version>'s recipe for the object's
component and language (config/recipes_<config>.bzl: CFLAGS+CWARNFLAGS, then INCFLAGS) with the object's own
additions (<component>/objects.bzl: copts after the flags, includes after the recipe's), followed by what this Xcode's
clang driver appends to every compile (its -Xclang vendor features, -mlinker-version, --target: asked of the driver
itself, so a new Xcode shows here). What cmake/XnuRecipes.cmake's switch_recipe did with make's records, from the
tables the XNU build itself compiles from.

A recipe is those arguments without -W flags (unless keep_warnings) and without arguments starting with a drop prefix;
with strip_includes, -I/-include/-isystem and their values go too (the freestanding recipe the vendored Linux code
uses). The tables' placeholders become the toolchain's: {xnu_obj} (XNU's object root as the headers-only tree the
drivers see: {gen} is <config>_ARM64_BCM2837/<component>/<config> in it, {export} its EXPORT_HDRS, {san} its san/),
{fakeroot} (the kernel SDK root: {sdkroot}), {xnu} and {tree}. Flags only Apple clang takes, or only code generation
needs, are listed apart (`apple`): the toolchain puts them in the apple_clang_only feature, which clang-tidy turns off.

usage: recipes.py --tables kernel/xnu-<version> --xcode-env F --out CONFIG=FILE... [--check] [--ios-min V]
                  --recipe NAME=OBJECT[:keep_warnings][:strip_includes][:drop=P]...
  OBJECT: an XNU object's source as objects.bzl names it (osfmk/arm/arm_init.c); its first directory is the component,
  its extension the language. --out: one file per XNU configuration (DEVELOPMENT=recipes.bzl, RELEASE=...).
  --check: compare with the files instead of writing them (the recipes test).
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common

# Apple clang only, or code generation only (r17-lint's list): clang-tidy (upstream clang) runs without them.
APPLE_PREFIXES = (
    '-fsanitize',
    '-flto',
    '-mcpu=',
    '-march=',
    '-mno-implicit-sme',
    '-fptrauth',
    '-ftrivial-auto-var-init',
    '-fbounds-safety',
    '-fexperimental-bounds-safety',
    '-fc++-static-destructors=',
    '-mlinker-version=',
)
APPLE_PAIRS = ('-mllvm', '-Xclang', '-index-store-path', '-arch')
LANGUAGES = {'.c': 'c', '.cpp': 'cxx', '.s': 'asm'}
# The flags every recipe targets with: what the driver's appended tail depends on (the iOS target is the XNU's).
PROBE = ['-arch', 'arm64', '-mios-version-min={ios_min}', '-mkernel']
# The compilation directory make's objects carried (the old layout's object root): the native XNU objects keep it
# (kernel/xnu-<version>/config/xnu_objects.bzl LEGACY), and so do the drivers.
MAKE_OBJ = '/switch-ios/bazel-out/darwin_arm64-fastbuild/bin/out/native-a57/root-v1/tree/build/xnu/ios-obj'
PLACEHOLDERS = ('{xnu_obj}', '{fakeroot}', '{xnu}', '{tree}')
UPDATE = 'bazel run //bazel/toolchains/kernel:update_recipes'


@dataclass(frozen=True)
class Options:
    """How a recipe filters its object's arguments (the --recipe suffixes)."""

    keep_warnings: bool = False
    strip_includes: bool = False
    drop: tuple[str, ...] = ()


def load_table(path: Path) -> dict:
    """Return the top-level names of a Starlark file of literals (the XNU build's tables: dicts, lists, struct)."""
    scope = {'struct': SimpleNamespace}
    text = '\n'.join(line for line in path.read_text().splitlines() if not line.startswith('load('))
    exec(compile(text, str(path), 'exec'), scope)
    return scope


def driver_tail(xcode_env: str, ios_min: str) -> list[str]:
    """Return what this Xcode's clang driver appends to a kernel compile's arguments (its compilation database's)."""
    xcode = common.xcode_env(xcode_env)
    with tempfile.TemporaryDirectory() as scratch:
        work = Path(scratch)
        (work / 'empty.c').write_text('int switch_recipe_probe;\n')
        clang = xcode['XCODE_TOOLCHAIN'] + '/clang'
        command = [
            clang,
            '-MJ',
            'db.json',
            '-isysroot',
            xcode['IPHONEOS_SDK'],
            *[p.format(ios_min=ios_min) for p in PROBE],
            '-c',
            'empty.c',
            '-o',
            'empty.o',
        ]
        subprocess.run(command, cwd=work, env=common.environment(xcode), check=True)
        arguments = json.loads((work / 'db.json').read_text().rstrip().rstrip(','))['arguments']
    return arguments[arguments.index('-c', arguments.index(PROBE[-1])) + 1 :]


def place(value: str, config: str, component: str) -> str:
    """Return a table argument with the toolchain's placeholders."""
    gen = f'{{xnu_obj}}/{config}_ARM64_BCM2837/{component}/{config}'
    if value == '{gen}/.':
        value = gen  # make's `-I .`
    for table, toolchain in (
        ('{gen}', gen),
        ('{export}', '{xnu_obj}/EXPORT_HDRS'),
        ('{san}', '{xnu_obj}/san'),
        ('{sdkroot}', '{fakeroot}'),
    ):
        value = value.replace(table, toolchain)
    rest = value
    for name in PLACEHOLDERS:
        rest = rest.replace(name, '')
    if '{' in rest:
        sys.exit(f'recipe argument with a placeholder the toolchain does not fill: {value}')
    return value


def filtered(arguments: list[str], options: Options) -> list[str]:
    """Return the arguments a recipe keeps."""
    out, skip = [], False
    for i, value in enumerate(arguments):
        if skip:
            skip = False
            continue
        if not options.keep_warnings and value.startswith('-W'):
            continue
        if (
            not options.keep_warnings
            and value == '-Xclang'
            and arguments[i + 1 : i + 2]
            and arguments[i + 1][:2] == '-W'
        ):
            skip = True
            continue
        if not value.startswith(options.drop or ('\0',)):
            out.append(value)
    if not options.strip_includes:
        return out
    stripped, skip = [], False
    for value in out:
        if skip:
            skip = False
        elif value in ('-I', '-include', '-isystem'):
            skip = True
        else:
            stripped.append(value)
    return stripped


def recipe(tables: Path, config: str, source: str, tail: list[str], options: Options) -> tuple[str, list[str]]:
    """Return (debug directory, arguments) of a driver compiled as XNU compiles `source`."""
    component, suffix = source.split('/', 1)[0], Path(source).suffix
    recipes = load_table(tables / 'config' / f'recipes_{config.lower()}.bzl')[f'{config}_RECIPES']
    objects = load_table(tables / component / 'objects.bzl')[config]
    if source not in objects.objects:
        sys.exit(f"{source} is not one of {component}'s {config} objects ({tables}/{component}/objects.bzl)")
    base = recipes[f'{component}_{LANGUAGES[suffix]}']
    arguments = base.flags + objects.copts.get(source, []) + base.includes + objects.includes.get(source, []) + tail
    out = [place(value, config, component) for value in filtered(arguments, options)]
    return f'{MAKE_OBJ}/{config}_ARM64_BCM2837/{component}/{config}', out


def split(arguments: list[str]) -> tuple[list[str], list[str]]:
    """Return the (portable, apple) argument lists, each in the recipe's order."""
    portable, apple, i = [], [], 0
    while i < len(arguments):
        value = arguments[i]
        if value in APPLE_PAIRS and i + 1 < len(arguments):
            apple += [value, arguments[i + 1]]
            i += 2
            continue
        (apple if value.startswith(APPLE_PREFIXES) else portable).append(value)
        i += 1
    return portable, apple


def render(recipes: dict, tables: str, config: str) -> str:
    """Return recipes.bzl's text."""
    lines = [
        '"""Generated by tools/kernel/recipes.py (`bazel run //bazel/toolchains/kernel:update_recipes`); checked by',
        '//bazel/toolchains/kernel:recipes_test. Do not edit.',
        '',
        f"The {config} compile recipes of code linked into the kernel, from {tables}'s recipe and object tables: the",
        "kernel toolchain's features (bazel/toolchains/kernel/config.bzl). Placeholders: "
        + ', '.join(PLACEHOLDERS)
        + '.',
        '"""',
        '',
        'RECIPES = {',
    ]
    for name, (directory, arguments) in recipes.items():
        portable, apple = split(arguments)
        lines.append(f'    {json.dumps(name)}: struct(')
        lines.append('        compiler = "clang",')
        lines.append(f'        debug_dir = {json.dumps(directory)},')
        for key, values in (('flags', portable), ('apple', apple)):
            lines.append(f'        {key} = [')
            lines += [f'            {json.dumps(v)},' for v in values]
            lines.append('        ],')
        lines.append('    ),')
    lines.append('}')
    return '\n'.join(lines) + '\n'


def main() -> None:
    """Write (or, with --check, compare) each configuration's recipes file."""
    parser = argparse.ArgumentParser()
    parser.add_argument('--tables', required=True)
    parser.add_argument('--xcode-env', required=True)
    parser.add_argument('--out', action='append', required=True, help='CONFIG=PATH, e.g. DEVELOPMENT=recipes.bzl')
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--ios-min', default='17.0', help="the XNU's iOS deployment target")
    parser.add_argument('--recipe', action='append', required=True)
    args = parser.parse_args()
    tables = Path(args.tables)
    tail = driver_tail(args.xcode_env, args.ios_min)
    specs = []
    for spec in args.recipe:
        name, _, rest = spec.partition('=')
        source, *suffixes = rest.split(':')
        options = Options(
            keep_warnings='keep_warnings' in suffixes,
            strip_includes='strip_includes' in suffixes,
            drop=tuple(s[len('drop=') :] for s in suffixes if s.startswith('drop=')),
        )
        specs.append((name, source, options))
    stale = []
    for target in args.out:
        config, _, name = target.partition('=')
        recipes = {n: recipe(tables, config, source, tail, options) for n, source, options in specs}
        text = render(recipes, args.tables, config)
        out = Path(name)
        if args.check:
            if out.read_text() != text:
                stale.append(name)
            continue
        if not out.is_absolute() and os.environ.get('BUILD_WORKSPACE_DIRECTORY'):
            out = Path(os.environ['BUILD_WORKSPACE_DIRECTORY']) / out
        out.write_text(text)
    if stale:
        sys.exit(f"{', '.join(stale)} out of date with {tables}'s tables: {UPDATE}")


if __name__ == '__main__':
    main()
