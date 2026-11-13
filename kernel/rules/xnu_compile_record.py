"""XNU's compile and link records as the tables kernel/xnu-<version>/ compiles from.

The tables are <component>/objects.bzl and config/recipes_<config>.bzl (the forms xnu_objects.bzl and xnu_kernel.bzl
read).

Input is a VERBOSE=YES make build of the tree (real compilers): every compile's -MJ record in the object root, each
component's <component>.filelist for the link order, and from the log the setsegname loops and the kernel's link.

usage: xnu_compile_record.py --log MAKE.LOG --obj OBJROOT --src SRC --sdkroot SDKROOT --tree TREE --config CONFIG
                             --out KERNEL/XNU-<VERSION>
"""

import argparse
import collections
import json
import re
import shlex
from collections.abc import Iterable
from pathlib import Path
from typing import NamedTuple

COMPONENTS = ['osfmk', 'bsd', 'iokit', 'libkern', 'libsa', 'pexpert', 'san', 'security']
LANGS = {'c': 'c', 'cpp': 'cxx', 's': 'asm'}
JOINED = ('-I', '-D', '-U')
SEGMENT_LOOP = re.compile(r'^for (\w+)_file in (.*?)\s*;\s*(?:do)?')
SEGMENT_STEP = re.compile(r'setsegname (.*?) -o \$\{(\w+)_file\}__ \$\{\2_file\}')


class Places:
    """Absolute paths of the build as the tables' placeholders, longest first."""

    def __init__(self, args: argparse.Namespace, objdir: str):
        obj = Path(args.obj).resolve()
        pairs = [
            (str(Path(args.src).resolve()), '{xnu}'),
            (str(obj / 'EXPORT_HDRS'), '{export}'),
            (str(obj / 'san'), '{san}'),
            (str(obj / objdir), '{objdir}'),
            (str(Path(args.sdkroot).resolve()), '{sdkroot}'),
            (str(Path(args.tree).resolve()), '{tree}'),
        ]
        self.pairs = sorted(pairs, key=lambda p: -len(p[0]))

    def spell(self, token: str, gen: str | None) -> str:
        """Token with every absolute root as its placeholder; gen (the compile's directory) as {gen}."""
        for root, name in [(gen, '{gen}'), *self.pairs]:
            token = token.replace(root + '/', name + '/').replace(root, name) if root else token
        return token


def split_joined(tokens: Iterable[str]) -> list[str]:
    """-Ifoo -> -I foo, -DX -> -D X, -UX -> -U X (the tables' spelling)."""
    out = []
    for t in tokens:
        if t.startswith(JOINED) and len(t) > 2 and not t.startswith('-Wl'):
            out += [t[:2], t[2:]]
        else:
            out.append(t)
    return out


def compiles(obj: str) -> list[tuple[Path, str, str, list[str]]]:
    """Return every compile's -MJ record in obj: (the directory it ran in, its source, its arguments).

    The records keep make's argument order less the dependency flags (-MD, -MF, -MJ, -MP), plus what the clang driver
    appends to every compile (vendor features, the linker version, the target): that common suffix is dropped, the
    kernel toolchain adds its own.
    """
    found = []
    for path in sorted(Path(obj).rglob('*.json')):
        text = path.read_text().strip().rstrip(',')
        if text.startswith('{'):
            record = json.loads(text)
            found.append((path, record['directory'], record['file'], record['arguments']))
    suffix = min(len(r[3]) for r in found)
    while suffix and len({tuple(r[3][-suffix:]) for r in found}) != 1:
        suffix -= 1
    return [(path, cwd, source, argv[: len(argv) - suffix]) for path, cwd, source, argv in found]


def parse_compile(argv: list[str], source: str, places: Places, gen: str) -> tuple[str, list[str], list[str]]:
    """(language, flags up to INCFLAGS, INCFLAGS and the object's own) of one compile record."""
    rest = [t for t in argv[1:] if t != source]
    if '-o' in rest:
        out = rest.index('-o')
        rest = rest[:out] + rest[out + 2 :]
    lang = LANGS[source.rpartition('.')[2]]
    rest = split_joined(rest)
    inc = next(i for i in range(len(rest) - 1) if rest[i] == '-I' and rest[i + 1] == '.')
    flags, includes = rest[:inc], rest[inc:]

    def spell(tokens: list[str]) -> list[str]:
        out = []
        for token in split_joined(tokens):
            if token == '.':
                t = '{gen}/.'
            elif token == 'meta_features.h':
                t = '{gen}/meta_features.h'
            else:
                t = token
            out.append(places.spell(t, gen))
        return out

    return lang, spell(flags), drop_missing_sdk_includes(spell(includes))


def drop_missing_sdk_includes(tokens: list[str]) -> list[str]:
    """Drop the iPhoneOS SDK's iBoot and AppleFeatures include directories make names: the SDK has neither."""
    out, i = [], 0
    while i < len(tokens):
        if tokens[i] == '-I' and i + 1 < len(tokens) and '/Platforms/iPhoneOS.platform/' in tokens[i + 1]:
            i += 2
            continue
        out.append(tokens[i])
        i += 1
    return out


def segments(log: str) -> dict[str, list[str]]:
    """Object name -> setsegname argument strings, in order (the loops make runs after a component's compiles)."""
    out = collections.defaultdict(list)
    files = {}
    for line in Path(log).read_text(errors='replace').splitlines():
        m = SEGMENT_LOOP.match(line.strip())
        if m:
            files[m.group(1)] = m.group(2).split()
            continue
        m = SEGMENT_STEP.search(line)
        if m:
            for f in files.get(m.group(2), []):
                out[f].append(m.group(1))
    return out


class LinkRecipes(NamedTuple):
    lto: list[str] | None
    final: list[str]


def shell_commands(text: str) -> Iterable[list[str]]:
    pending = ''
    for line in text.splitlines():
        pending += line.strip()
        if pending.endswith('\\'):
            pending = pending[:-1] + ' '
            continue
        if '-filelist' not in pending:
            pending = ''
            continue
        lexer = shlex.shlex(pending, posix=True, punctuation_chars=';&|')
        lexer.whitespace_split = True
        command = []
        for token in lexer:
            if token and all(c in ';&|' for c in token):
                if command:
                    yield command
                    command = []
            else:
                command.append(token)
        if command:
            yield command
        pending = ''
    if '-filelist' in pending:
        raise SystemExit('unfinished shell continuation in the make log')


def link_command(command: list[str]) -> tuple[str, list[str]] | None:
    if '-filelist' not in command:
        return None
    at = command.index('-filelist')
    listing = Path(command[at + 1]).name if at + 1 < len(command) else ''
    if listing not in ('link.filelist', 'ltolink.filelist', 'nonltolink.filelist'):
        return None
    compiler = re.compile(r'(clang(?:\+\+)?(?:-[0-9]+)?|cc|c\+\+|ld)$')
    executable = next((i for i, word in enumerate(command[:at]) if compiler.fullmatch(Path(word).name)), None)
    if executable is None:
        return None
    argv = command[executable + 1 :]
    if listing == 'ltolink.filelist':
        if '-r' not in argv:
            raise SystemExit('the LTO link is not relocatable')
        return listing, argv
    output = argv.index('-o') if '-o' in argv else len(argv)
    if output + 1 >= len(argv) or '.unstripped' not in argv[output + 1]:
        return None
    if '-lcc_kext' not in argv:
        raise SystemExit('the kernel link has no -lcc_kext archive boundary')
    return listing, [*argv[: argv.index('-lcc_kext') + 1], '@KERNEL_ARCHIVES@']


def link(log: str, places: Places) -> LinkRecipes:
    candidates: dict[str, list[list[str]]] = collections.defaultdict(list)
    for command in shell_commands(Path(log).read_text(errors='replace')):
        parsed = link_command(command)
        if parsed:
            listing, argv = parsed
            if argv not in candidates[listing]:
                candidates[listing].append(argv)
    lto = candidates['ltolink.filelist']
    final = candidates['link.filelist']
    if lto:
        # Make prints both branches of its shell conditional. Bazel links the single justlto.o output.
        final = [
            args for args in candidates['nonltolink.filelist'] if any(Path(arg).name == 'justlto.o' for arg in args)
        ]
    if len(lto) > 1 or len(final) != 1:
        raise SystemExit(f'expected one kernel link and at most one LTO link; found {len(final)} and {len(lto)}')

    def spell(args: list[str]) -> list[str]:
        return [places.spell(arg, None) for arg in args]

    return LinkRecipes(spell(lto[0]) if lto else None, spell(final[0]))


def gen_root(cwd: str, objdir: str) -> Path:
    """Return the directory a component's generated sources hang from: its <component>/<CONFIG>, or the config's."""
    path = Path(cwd).resolve()
    while objdir not in (path.name, path.parent.parent.name):
        path = path.parent
    return path


def common_prefix(values: list[list[str]]) -> list[str]:
    """Return the longest list every value starts with: a component's recipe is what all its objects share."""
    first = values[0]
    n = len(first)
    for v in values[1:]:
        n = min(n, len(v))
        while v[:n] != first[:n]:
            n -= 1
    return first[:n]


def bzl_list(values: Iterable[str], indent: int) -> str:
    pad = ' ' * indent
    return '[\n' + ''.join(f'{pad}    {_q(v)},\n' for v in values) + f'{pad}]'


def _q(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


class ObjectTable(NamedTuple):
    objects: list[str]
    copts: dict[str, list[str]]
    includes: dict[str, list[str]]
    segments: dict[str, list[str]]


def write_objects(path: Path, comp: str, config: str, table: ObjectTable) -> None:
    doc = (
        f'"""XNU\'s {comp} objects for each XNU config, in link order (make\'s {comp}.filelist), and what each one adds'
        " to its\ncomponent's recipe: copts (conf/Makefile.*'s <object>_CFLAGS_ADD and _CWARNFLAGS_ADD), includes"
        " (<object>_INCFLAGS,\nafter the recipe's) and segments (the setsegname passes make runs on the object).\n"
        '"gen:" sources are generated (MIG, iig, makesyscalls, newvers) in the component\'s object directory.\n'
        'Generated by kernel/rules/xnu_compile_record.py."""\n\n'
    )
    text = doc + f'{config} = struct(\n    objects = {bzl_list(table.objects, 4)},\n'
    text += f'    copts = {bzl_dict(table.copts)},\n    includes = {bzl_dict(table.includes)},\n'
    text += f'    segments = {bzl_dict(table.segments)},\n)\n'
    path.write_text(text)


def bzl_dict(d: dict[str, list[str]]) -> str:
    if not d:
        return '{}'
    body = ''.join(f'        {_q(k)}: {bzl_list(v, 8)},\n' for k, v in sorted(d.items()))
    return '{\n' + body + '    }'


def compiled_objects(
    obj: str, src_root: str, objdir: str, places: Places
) -> dict[tuple[str, str], tuple[str, str, list[str], list[str]]]:
    by_object = {}
    for path, cwd, source, argv in compiles(obj):
        gen = str(Path(cwd).resolve())
        lang, flags, inc = parse_compile(argv, source, places, gen)
        comp = 'config' if gen.endswith('/' + objdir) else Path(gen).parent.name
        resolved = str(Path(cwd, source).resolve())
        if resolved.startswith(src_root + '/'):
            entry = resolved[len(src_root) + 1 :]
        else:
            entry = 'gen:' + str(Path(resolved).relative_to(gen_root(cwd, objdir)))
        by_object[(comp, path.name[: -len('.json')])] = (entry, lang, flags, inc)
    return by_object


def link_order(obj: str, objdir: str, config: str, comp: str) -> list[str]:
    if comp == 'config':
        return ['lastkerneldataconst.o', 'lastkernelconstructor.o', 'nonlto.o', 'version.o']
    filelist = Path(obj) / objdir / comp / config / f'{comp}.filelist'
    return [Path(p).name for p in filelist.read_text().split()]


def object_table(
    comp: str,
    key: str,
    rows: list[tuple[str, str, str, list[str], list[str]]],
    recipes: dict[str, tuple[list[str], list[str]]],
    segs: dict[str, list[str]],
) -> ObjectTable:
    table = ObjectTable([], {}, {}, {})
    for name, entry, lang, flags, inc in rows:
        base_flags, base_inc = recipes[f'{key}_{lang}']
        if flags[: len(base_flags)] != base_flags:
            raise SystemExit(f'{comp} {name}: flags differ from the {key}_{lang} recipe beyond its additions')
        if inc[: len(base_inc)] != base_inc:
            raise SystemExit(f'{comp} {name}: includes differ from the {key}_{lang} recipe')
        table.objects.append(entry)
        if flags[len(base_flags) :]:
            table.copts[entry] = flags[len(base_flags) :]
        if inc[len(base_inc) :]:
            table.includes[entry] = inc[len(base_inc) :]
        if name in segs:
            table.segments[entry] = segs[name]
    return table


def recipes_text(config: str, recipes: dict[str, tuple[list[str], list[str]]], links: LinkRecipes) -> str:
    lines = [
        (
            f'"""XNU\'s compile recipes for config {config}: per component and language, CFLAGS+CWARNFLAGS (flags) and'
            ' INCFLAGS\n(includes). Generated by kernel/rules/xnu_compile_record.py."""'
        ),
        '',
        f'{config}_RECIPES = {{',
    ]
    for k in sorted(recipes):
        flags, inc = recipes[k]
        lines.append(
            f'    {_q(k)}: struct(\n        flags = {bzl_list(flags, 8)},\n'
            f'        includes = {bzl_list(inc, 8)},\n    ),'
        )
    lines.append('}')
    if links.lto is not None:
        lines += ['', f'{config}_LINK_LTO = {bzl_list(links.lto, 0)}']
    lines += ['', f'{config}_LINK = {bzl_list(links.final, 0)}']
    return '\n'.join(lines) + '\n'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('log', 'obj', 'src', 'sdkroot', 'tree', 'config', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    config = args.config
    objdir = f'{config}_ARM64_BCM2837'
    places = Places(args, objdir)
    by_object = compiled_objects(args.obj, str(Path(args.src).resolve()), objdir, places)
    segs = segments(args.log)
    recipes = {}
    for comp in [*COMPONENTS, 'config']:
        rows = [(n, *by_object[(comp, n)]) for n in link_order(args.obj, objdir, config, comp)]
        key = 'kernel' if comp == 'config' else comp
        for lang in sorted({r[2] for r in rows}):
            mine = [r for r in rows if r[2] == lang]
            recipes[f'{key}_{lang}'] = (common_prefix([r[3] for r in mine]), common_prefix([r[4] for r in mine]))
        write_objects(Path(args.out) / comp / 'objects.bzl', comp, config, object_table(comp, key, rows, recipes, segs))
    recipes_path = Path(args.out) / 'config' / f'recipes_{config.lower()}.bzl'
    recipes_path.write_text(recipes_text(config, recipes, link(args.log, places)))
    print(f'{len(by_object)} compiles, {len(recipes)} recipes')


if __name__ == '__main__':
    main()
