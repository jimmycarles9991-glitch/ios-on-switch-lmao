#!/usr/bin/env python3
"""Make the driver's pipeline pack (drivers/TegraGM20BMetal/pack/make_pack.py) beside the seed.

One action per step (gpu/seed/tools.bzl seed_tools builds, gpu/seed/defs.bzl seed_pipeline_pack runs the rest):

    make_pack.py build --xcode-env F --translate <darwin translation .a> --compile <switch_compile.o>
                     --metal-abi <MetalABI.h> --context-header <SwitchGM20BContext.h> --generator <pack/make_pack.py>
                     --tree <tree> --out <dir>
        the generator's host tools (directpack and native-seed over the translation and the stage compile, the mock
        GM20B, the driver's own.metallib) into <dir>, with the context init nvdec ran (context.log)
    make_pack.py corpus --corpus corpus.log --build <IPSW build> [--archives <build>.tsv]
                      [--app-states <build>.log] --out <file> --report <file>
        the corpus lines directpack makes (pack/directpack.mm: Core Animation's render states, without lib=, and
        every other library's the runtime looks up in the pack, framework_line), so an edit to any other line leaves
        the run's input as it was; with the render states Apple's archives declare for the
        device (precompile/archives/<build>.tsv) whose functions are one framework's default.metallib, and our own
        apps' states the pack holds (precompile/app-states/<build>.log, app_line). Every such line
        names the OS builds it is a state of (os=<build>[,<build>...] in its head): one of other builds is left out and
        counted in <report>, which the link puts at the end of the pack's report, but for a Core Animation line, kept
        with its tag for its name; one naming none fails the step. A state of two-plane YCbCr attachments is taken as
        the one the runtime makes for it (stand_in)
    make_pack.py split --generator <make_pack.py> --qc default.metallib --corpus <file> [--ca-names <file>]
                     [--ca-preferences <stage-tegra-gm20b-metal.py>]
                     --out <bucket corpus>...
        the states of each of the generator's buckets, with the Core Animation states a device's logs named
        (precompile/ca-names/<build>.txt)
    make_pack.py link --generator <make_pack.py> --out <dir> [--corpus-report <file>] [--archives <build>.tsv]
                    <bucket dir>...
        the pack made from the buckets' records, unstamped: <out>/pack.bin and report.txt, with the archive
        inventory's per-archive declared states (pack/archives.py)
    make_pack.py stamp --generator <make_pack.py> --pack <pack.bin> --manifest <MANIFEST.json> --out <dir>
        <out>/pack.bin with the tools' stack key in its header

Each bucket is its own step and tool (probe.py). Only the stamp reads the stack key, which every raw/ source changes:
a raw runtime edit reruns the stamp alone (but for the direct compile's own sources, which the tools build from), and a
bucket runs again only when its states, the tools (directpack holds the stage compile) or QuartzCore's library change.
No step but build runs a compiler, so the image builder runs them with the release's prebuilt tools.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROBE_LABEL = ' label=com.apple.coreanimation.'
LIBRARY = re.compile(r' lib=\S+')
# A function a state links (" | <vl|vp|fl|fp> <function> lib=<library> ..."), which any library may hold
LINKED_LIBRARY = re.compile(r'( \| [vf][lp] \S+) lib=\S+')
FRAMEWORK_LIBRARY = re.compile(r' lib=/System/Library/(?:Private)?Frameworks/(\w+)\.framework/default\.metallib')
# A bundle's library or a framework's resource library (VideoProcessors' *.bundle, AppleCVAPhoto's Resources/): the
# runtime keys its states by their functions' AIR
OS_LIBRARY = re.compile(r' lib=/System/Library/\S+?\.(?:bundle|framework/Resources)/\S+\.metallib')
QUARTZCORE = '/QuartzCore.framework/'
STAGE_LIBRARY = re.compile(r' \| [vf] \S+( lib=\S+)')
# A state of no fragment function (TegraGM20BMetalStates.mm packs it from its vertex function's library alone)
NO_FRAGMENT = re.compile(r' \| f - ')
# Our own apps' libraries (SwitchHWTest's): the runtime keys their states by their functions' AIR, as a bundle's; the
# pack holds only those precompile/app-states names, since a test of a state made at its creation must find none
APP_LIBRARY = re.compile(r' lib=/Applications/Switch\w*\.app/default\.metallib')
# The OS builds a line is a state of, in its head: an R line's (before " | v ") or a D line's
OS_BUILD = re.compile(r' os=(\S+)')
# A colour attachment of an R line's head: its index, MTLPixelFormat, and every field after the format (write mask,
# blending, the RGB source and destination factors and operation, the alpha ones)
COLOR_FIELDS = re.compile(r' c(\d)=(\d+):(\d+(?::\d+){7})(?= |$)')
# ycbcr_store.h: the formats whose attachments TegraGM20BMetalYcbcrTargets.mm standInTargetState takes
# (ycbcrRenderedFormat: two-plane YCbCr, and R10Unorm_PACKED and RG10Unorm_PACKED), and those of them it renders into
# the RGBA32Float stand-in (ycbcrTargetRefusal, kYcbcrTargetStandIn)
YCBCR_TARGETS = frozenset(range(500, 549)) - {542} | {588, 589}
YCBCR_RENDERED = frozenset(
    {500, 502, 503, 505, 506, 507, 508, 509, 510, 520, 522, 523, 525, 526, 527, 528, 529, 530}
    | {540, 541, 543, 544, 545, 546, 547, 548, 588, 589}
)
YCBCR_STAND_IN = 125
# ycbcrStandInFormat: the packed planes' R32Float and RG32Float, of their fragments' one or two components
STAND_INS = {588: 55, 589: 105}
# format_standin.h formatStandIn: the formats the GM20B renders none of that a stand-in of another
# format renders, and those of them that hold alpha (formatHoldsAlpha); not the extended-range ones (550 to 555), whose
# states are the descriptor's own, which the translator lowers onto their stand-in
FORMAT_STAND_INS = {11: 71, 31: 71, 41: 125, 42: 125, 44: 125, 84: 70, 85: 71, 93: 125}
HOLDS_ALPHA = frozenset({41, 42, 44, 84, 85})
# standin_blend.h alphaOneFactor: MTLBlendFactorDestinationAlpha, OneMinusDestinationAlpha and
# SourceAlphaSaturated where the destination alpha is 1, of a colour blend and of an alpha blend
ALPHA_ONE_RGB = {8: 1, 9: 0, 10: 0}
ALPHA_ONE_ALPHA = {8: 1, 9: 0, 10: 1}


def framework_line(line: str) -> bool:
    """Whether the runtime looks the line's state up in the pack (TegraGM20BMetalStates.mm pipelineFor).

    A vertex and a fragment function (or a vertex function and none) of one framework's default.metallib, and every
    function it links a framework
    library's: under its framework; or of one bundle or framework resource library (OS_LIBRARY), linking none: under
    its functions' AIR (TegraGM20BMetalRenderKey.h). Not QuartzCore's, whose states the pack makes from their names
    (make_pack.py ca_states).
    """
    stages = STAGE_LIBRARY.findall(line)
    frameworks = {m.group(1) for lib in stages if (m := FRAMEWORK_LIBRARY.fullmatch(lib))}
    linked = [lib for lib in LIBRARY.findall(line) if lib not in stages]
    if len(stages) != (1 if NO_FRAGMENT.search(line) else 2) or len(set(stages)) != 1:
        return False
    if not frameworks:
        return not LINKED_LIBRARY.search(line) and bool(OS_LIBRARY.fullmatch(stages[0])) and QUARTZCORE not in stages[0]
    return frameworks != {'QuartzCore'} and all(FRAMEWORK_LIBRARY.fullmatch(lib) for lib in linked)


def app_line(line: str) -> bool:
    """Whether an R line's vertex and fragment functions are one of our own apps' library's, linking nothing."""
    stages = STAGE_LIBRARY.findall(line)
    return (
        line.startswith('R ok=1 ')
        and len(stages) == 2
        and len(set(stages)) == 1
        and bool(APP_LIBRARY.fullmatch(stages[0]))
        and not LINKED_LIBRARY.search(line)
    )


def probe_lines(corpus: str) -> list[str]:
    """Return the corpus lines directpack.mm makes, as it reads them (a D line keeps its libraries).

    Not a dispatch shape of our own apps' kernels: their tests ask for kernels no pack holds (gpu.gm20b.archive_miss,
    gpu.gm20b.kernel_direct), and the probe's root holds their library.
    """
    return [
        line if framework_line(line) or line.startswith('D ') else LIBRARY.sub('', line)
        for line in corpus.split('\n')
        if (line.startswith('R ') and ' ok=1 ' in line and (PROBE_LABEL in line or framework_line(line)))
        or (line.startswith('D ok=1 ') and not APP_LIBRARY.search(line))
    ]


def alpha_one(fields: str) -> str:
    """Give a colour attachment's key fields after its format as a destination alpha of 1 reads them.

    The fields are its mask, blending, the RGB factors and operation, then the alpha ones; its factors of the
    destination alpha become the constants they come to (alphaOneFactor).
    """
    f = fields.split(':')
    for k, table in ((2, ALPHA_ONE_RGB), (3, ALPHA_ONE_RGB), (5, ALPHA_ONE_ALPHA), (6, ALPHA_ONE_ALPHA)):
        f[k] = str(table.get(int(f[k]), int(f[k])))
    return ':'.join(f)


def stand_in(line: str) -> str:
    """Give the line of the state the runtime makes for a render state of attachments it renders through a stand-in.

    TegraGM20BMetalYcbcrTargets.mm standInTargetState makes it with each such attachment's format the stand-in's: a
    two-plane YCbCr attachment's (codes, converted, sRGB or packed, 8- or 10-bit, or a packed 10-bit plane; one of a
    format no target texture is made of too, whose state draws no pass), blended, channel-masked or not, and a format
    the GM20B renders none of (FORMAT_STAND_INS); the blend factors of the destination alpha of a format that holds none
    become the constants they come to (gm20bAlphaOneBlend). The archives' declared keys stay the app's
    (pack/archives.py).
    """
    head, bar, tail = line.partition(' | ')
    targets = [m for m in COLOR_FIELDS.finditer(head) if int(m[2]) in YCBCR_TARGETS | FORMAT_STAND_INS.keys()]
    if not targets:
        return line
    for m in reversed(targets):
        fmt = int(m[2])
        if fmt in FORMAT_STAND_INS:
            fields = FORMAT_STAND_INS[fmt], m[3] if fmt in HOLDS_ALPHA else alpha_one(m[3])
        else:
            fields = STAND_INS.get(fmt, YCBCR_STAND_IN), alpha_one(m[3])
        head = head[: m.start(2)] + f'{fields[0]}:{fields[1]}' + head[m.end(3) :]
    return head + bar + tail


def stand_in_lines(lines: list[str]) -> list[str]:
    """Give `lines` through stand_in, a line it maps left out where an earlier line is already that state's.

    The archives declare one state for each of a pass's code formats (VideoStabilizationV2's fragmentBlur into
    RGB8_420_2P, RGB10_420_2P and RGB10_420_2P_PACKED), which the stand-in makes one state.
    """
    out, seen = [], set()
    for line in lines:
        mapped = stand_in(line)
        if mapped == line or mapped not in seen:
            out.append(mapped)
            seen.add(mapped)
    return out


def app_states(text: str) -> list[str]:
    """Return an app-states file's lines, each an app_line; exit naming the first that is not one."""
    lines = [line.rstrip() + ' ' for line in text.splitlines() if line.strip()]
    for line in lines:
        if not app_line(line):
            sys.exit(f"not one of our own apps' render states, linking nothing: {line[:300]}")
    return lines


def own_build(lines: list[str], build: str) -> tuple[list[str], list[str]]:
    """Give the lines a pack for IPSW `build` takes, without their os= tag, and the report's lines.

    A line tagged with other builds only is theirs: its encodings (RenderBox's c1 format, QuartzCore's constant sets)
    are not this one's, so it is left out; but a Core Animation render line of theirs is kept, tag and all, for its
    name, which names a state this build's QuartzCore makes too (the generator makes it from the name with this IPSW's
    tables, pack/make_pack.py ca_states). A compute line (C, D) names a kernel, not a state a name makes: another
    build's is left out like any other line (21H461's QuartzCore has no compute_sum_luma). A line with no tag names no
    build, and none is guessed for it: the step fails (precompile/harvest.py and tools/gpu/logharvest.py tag what they
    add).
    """
    taken, named, other, untagged = [], [], {}, []
    for line in lines:
        tag = OS_BUILD.search(line.split(' | ', 1)[0])
        if tag is None:
            untagged.append(line)
            continue
        builds = tag[1].split(',')
        if build in builds:
            taken.append(OS_BUILD.sub('', line, count=1))
            continue
        if line.startswith('R ') and PROBE_LABEL in line:
            named.append(line)
        for b in builds:
            other[b] = other.get(b, 0) + 1
    if untagged:
        sys.exit(f'{len(untagged)} corpus lines name no OS build (os=), the first: {untagged[0][:300]}')
    builds = ', '.join(f'{b} {n}' for b, n in sorted(other.items())) or 'none'
    left = len(lines) - len(taken)
    report = (
        f'corpus for {build}: {len(taken)} lines taken; {left} of other builds left out ({builds}), of them '
        f'{len(named)} Core Animation lines kept for their names'
    )
    return taken + named, [report]


def archive_lines(inventory: str) -> list[str]:
    """Return the render states an archive inventory (corpus/tools/archives/declared.py) has the pack make.

    Those declared for the device's GPU families that the runtime looks up in the pack (framework_line: a
    framework's default.metallib's, or a bundle's by its functions' AIR);
    QuartzCore's archive is its offline compilation manifest (pack/ca/caset.py's manifest tier). They keep lib=,
    which the probe loads them from.
    """
    out = []
    for row in inventory.splitlines():
        kind, _owner, _source, family, line = row.split('\t')
        if kind == 'render' and family == 'device' and framework_line(line):
            out.append(line)
    return out


def generate(generator: list[str], arguments: list[str | Path], env: dict[str, str], what: str) -> str:
    """Run the generator; exit with its output's tail if it fails."""
    result = subprocess.run([*generator, *map(str, arguments)], env=env, capture_output=True, text=True, check=False)
    if result.returncode:
        sys.exit(f'{what} failed:\n{(result.stdout + result.stderr)[-6000:]}')
    return result.stdout


def fresh(path: Path) -> Path:
    """Return `path` as an empty directory."""
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True)
    return path


def build(args: argparse.Namespace, generator: list[str], env: dict[str, str], root: Path) -> None:
    xcode = dict(line.split('=', 1) for line in Path(args.xcode_env).read_text().splitlines() if '=' in line)
    env['DEVELOPER_DIR'] = xcode['DEVELOPER_DIR']
    arguments = ['--build-only', '--translate', root / args.translate, '--compile', root / args.compile]
    arguments += ['--metal-abi', root / args.metal_abi]
    arguments += ['--context-header', root / args.context_header]
    arguments += ['--out', root / args.out]
    generate(generator, arguments, env, "the pack generator's tools")


def split(args: argparse.Namespace, generator: list[str], env: dict[str, str], work: Path) -> None:
    arguments = ['--split', '--qc', Path(args.qc).resolve(), '--corpus', Path(args.corpus).resolve(), '--out', work]
    arguments += ['--ca-names', Path(args.ca_names).resolve()] if args.ca_names else []
    arguments += ['--ca-preferences', Path(args.ca_preferences).resolve()] if args.ca_preferences else []
    print(generate(generator, arguments, env, 'the split into buckets').rstrip())
    made = sorted((work / 'work').glob('bucket*'), key=lambda d: int(d.name.removeprefix('bucket')))
    if len(made) != len(args.out):
        sys.exit(f'the generator made {len(made)} buckets, the build declares {len(args.out)}')
    for bucket, out in zip(made, args.out, strict=True):
        shutil.copyfile(bucket / 'corpus.log', out)


def link(args: argparse.Namespace, generator: list[str], env: dict[str, str], work: Path) -> None:
    linked = fresh(work / 'linked')
    inventory = ['--archives', Path(args.archives).resolve()] if args.archives else []
    selectors = ['--ca-selectors', Path(args.ca_selectors).resolve()] if args.ca_selectors else []
    arguments = ['--link', '--out', linked, *inventory, *selectors, *[Path(b).resolve() for b in args.buckets]]
    report = generate(generator, arguments, env, 'link')
    if args.corpus_report:
        corpus = Path(args.corpus_report).read_text()
        report += corpus
        with open(linked / 'report.txt', 'a') as f:
            f.write(corpus)
    # Every state left out has its own line, megabytes of them: Bazel shows none of an action's output past 1 MiB, so
    # the summary comes here and the lines stay in report.txt (the link's output group).
    skipped = [line for line in report.splitlines() if line.startswith('  skipped')]
    print('\n'.join(line for line in report.splitlines() if not line.startswith('  skipped')))
    print(f'{len(skipped)} states left out, each named in report.txt')
    out = fresh(Path(args.out))
    # pack.json, layouts.txt and programs.bin name the pack's programs and set-0 layouts for nvdec (test/eq)
    for name in ('pack.bin', 'report.txt', 'pack.json', 'layouts.txt', 'programs.bin'):
        shutil.copyfile(linked / name, out / name)


def stamp(args: argparse.Namespace, generator: list[str], env: dict[str, str], _work: Path) -> None:
    stack = json.loads(Path(args.manifest).read_text())['stack']
    out = fresh(Path(args.out))
    shutil.copyfile(args.pack, out / 'pack.bin')
    generate(generator, ['--stamp', out / 'pack.bin', '--stack', stack], env, 'the stamp')
    print(f'raw pack: stack {stack}')


STEPS = {
    'build': (
        'xcode-env',
        'translate',
        'compile',
        'metal-abi',
        'context-header',
        'generator',
        'tree',
        'out',
    ),
    'split': ('generator', 'qc', 'corpus'),
    'link': ('generator', 'out'),
    'stamp': ('generator', 'pack', 'manifest', 'out'),
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    steps = parser.add_subparsers(dest='step', required=True)
    corpus = steps.add_parser('corpus')
    for name in ('corpus', 'out'):
        corpus.add_argument('--' + name, required=True)
    corpus.add_argument('--archives')
    corpus.add_argument('--app-states')
    corpus.add_argument('--build', required=True)
    corpus.add_argument('--report', required=True)
    for step, names in STEPS.items():
        p = steps.add_parser(step)
        for name in names:
            p.add_argument('--' + name, required=True)
    steps.choices['split'].add_argument('--out', nargs='+', required=True)
    steps.choices['split'].add_argument('--ca-names')
    steps.choices['split'].add_argument('--ca-preferences')
    steps.choices['link'].add_argument('buckets', nargs='+')
    steps.choices['link'].add_argument('--corpus-report')
    steps.choices['link'].add_argument('--archives')
    steps.choices['link'].add_argument('--ca-selectors')
    args = parser.parse_args()
    if args.step == 'corpus':
        lines, report = own_build(probe_lines(Path(args.corpus).read_text()), args.build)
        if args.archives:
            harvested = set(lines)
            lines += [line for line in archive_lines(Path(args.archives).read_text()) if line not in harvested]
        lines = stand_in_lines(lines)
        if args.app_states:
            own = app_states(Path(args.app_states).read_text())
            lines += own
            report.append(f"our own apps' states: {len(own)} lines ({Path(args.app_states).name})")
        Path(args.out).write_text(''.join(line + '\n' for line in lines))
        Path(args.report).write_text(''.join(line + '\n' for line in report))
        print('\n'.join(report))
        return
    root = Path.cwd()
    work = fresh(root / f'_make_pack_{args.step}_{os.getpid()}')
    (work / 'home').mkdir()
    env = {
        'PATH': '/usr/bin:/bin',
        'ZERO_AR_DATE': '1',
        'HOME': str(work / 'home'),
        'TMPDIR': os.environ.get('TMPDIR', str(work)),
        'PACK_TREE': str(root / getattr(args, 'tree', '.')),
    }
    generator = [sys.executable, str(root / args.generator)]
    {'build': build, 'split': split, 'link': link, 'stamp': stamp}[args.step](
        args, generator, env, root if args.step == 'build' else work
    )
    shutil.rmtree(work)


if __name__ == '__main__':
    main()
