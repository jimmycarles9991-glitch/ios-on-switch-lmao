#!/usr/bin/env python3
"""make_pack: the driver's pipeline pack (runtime/pack.h), made on this Mac.

Core Animation's render pipeline states (PROBE_LABEL: the render server's) are made as the runtime makes a state the
pack lacks: the translator (the translation's offline compile) translates each, and the stage compile
(third_party/mesa/src/nouveau/switch_compile, linked in) compiles its stages, with no device, pipeline, draw or capture
(directpack.mm, runtime/TegraGM20BMetalDirect.h).
The harvested Core Animation lines go in as drawn, beside the states made from their names (ca/caset.py, ca/caspec.py:
QuartzCore's own decoding of a name into a state, with this IPSW's tables, ca/qc-<build>.json): QuartzCore's offline
compilation manifest, and the closure over the blend CA puts in the pipeline and over the blend it leaves in the
fragment (CA_TIERS).

  1. directpack.mm makes every state's record: its programs, the 3D registers emit makes for it, its push words and
     its set-0 layout; qualified ordered framebuffer-fetch records retain their barrier twins; and every compute
     dispatch shape's kernel (runtime/direct_compute.h);
  2. context_state.cpp runs the channel's context init (kits/SwitchGM20BEmit's) through nvdec (pack/nvdec.cpp, Mesa's
     Fermi MME simulator in pack/mesa): the registers every record is laid over, with draw_context.txt;
  3. directlink.py writes pack.bin by mkpack.py's rules: per state its programs, registers, push words and layout, plus
     the context init with the addresses to relocate.

usage: make_pack.py --translate <host libnative_metal_translate_static.a> --compile <host switch_compile.o>
                  --metal-abi <MetalABI.h> --qc <default.metallib> --corpus <corpus.log> --out <dir>
                  [--stack <hex key>] [--prebuilt <dir>]
       make_pack.py --build-only --translate <host libnative_metal_translate_static.a> --compile <host switch_compile.o>
                  --metal-abi <MetalABI.h> --context-header <SwitchGM20BContext.h> --out <dir>
writes <out>/pack.bin, pack.json, layouts.txt, report.txt and keeps the buckets in <out>/work (deleted with --clean).
--build-only compiles directpack, native-seed, the mock GPU (libmock_iokit.dylib) and own.metallib into <dir>, with the
channel's context init as nvdec runs it (context.log), then stops;
--prebuilt <dir> runs those instead of compiling them (//gpu/seed:local_raw_tools makes them its own action, so a corpus
edit only reruns the buckets).
--ca-names: Core Animation states a device's logs name as missing from the pack (precompile/ca_names.py), made from
their names in the corpus tier, as the journal's own are.
--ca-preferences: the GPU stager (stage-tegra-gm20b-metal.py, the default) whose Core Animation preferences decide the
colour precision the device's QuartzCore names its states with (ca/deepcolour.py).
--stack: the build's stack key (the plugin is compiled with the same value), stamped into the pack header: the runtime
refuses a pack of another build.
The same work in three steps, for a build that caches each bucket's run (it depends on the bucket's state lines and the
tools alone):
       make_pack.py --split --qc <default.metallib> --corpus <corpus.log> [--ca-names <ca-names/<build>.txt>]
                  [--ca-preferences <stager>] --out <dir>
           writes <dir>/work/bucket0 .. bucket16, each a corpus.log of its states (bucket16 our own apps')
       directbucket.py --probe-bucket <bucket dir> --prebuilt <dir> --qc <default.metallib> [--root <dir>]
                       [--data-libs <dir>]
           makes one bucket in place (directbucket.py)
       make_pack.py --link --out <dir> [--stack <hex key>] [--archives <inventory.tsv>] <bucket dir>...
           directlink.py over the buckets: <dir>/pack.bin and the rest, as the one-step form writes them, with the
           archive inventory's declared states (archives.py)
       make_pack.py --stamp <pack.bin> --stack <hex key>
           the stack key into a linked pack's header, so a build stamps a pack it linked without one
"""

import concurrent.futures
import contextlib
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import NamedTuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packfile

HERE = Path(__file__).resolve().parent  # drivers/TegraGM20BMetal/pack
METAL = HERE.parent
TREE = Path(os.environ.get('PACK_TREE', METAL.parents[1]))  # tree/ (drivers/TegraGM20BMetal -> tree)
PLUGIN = TREE / 'drivers/TegraGM20BMetal/plugin'
ABI = TREE / 'drivers/TegraGM20B'
CLASSES = TREE / 'third_party/mesa/src/nouveau/headers'  # NVIDIA's class headers find nvtypes.h here (nvdec.cpp)
STAGER = METAL / 'stage-tegra-gm20b-metal.py'


def run(cmd: list[str | Path], env: dict[str, str] | None = None, out: Path | None = None) -> str:
    with open(out, 'w') if out else contextlib.nullcontext() as stdout:
        r = subprocess.run(
            [str(c) for c in cmd],
            capture_output=out is None,
            text=True,
            env=env,
            stdout=stdout,
            stderr=subprocess.PIPE if out else None,
            check=False,
        )
    if r.returncode:
        sys.exit(
            f'make_pack: FAILED: {" ".join(map(str, cmd))[:400]}\n{(r.stdout or "")[-3000:]}{(r.stderr or "")[-5000:]}'
        )
    return (r.stdout or '') + (r.stderr or '')


def option(name: str, required: bool = True) -> str | None:
    if name in sys.argv[1:-1]:
        return sys.argv[sys.argv.index(name) + 1]
    if required:
        sys.exit(__doc__)
    return None


PACK_TOOLS = ('directpack', 'native-seed', 'libmock_iokit.dylib', 'own.metallib', 'context.log')
# Another framework's states, a bundle's or a framework resource library's, and our own apps' (their lines keep lib=,
# tools/gpu/make_pack.py framework_line and app_line): the probe loads that library from the crawl's root
FRAMEWORK = re.compile(
    r' lib=(?:/System/Library/(?:(?:Private)?Frameworks/\w+\.framework/default'
    r'|\S+?\.(?:bundle|framework/Resources)/\S+)|/Applications/Switch\w*\.app/default)\.metallib '
)
LIBRARY = re.compile(r' lib=(\S+)')
# A function a state links (" | <vl|vp|fl|fp> <function> lib=<library> ..."): its library is not the state's
LINKED_LIBRARY = re.compile(r'( \| [vf][lp] \S+) lib=\S+')
LINKED = ('vl ', 'vp ', 'fl ', 'fp ')
# What the journal names a stage's function with besides its library function (NativeBridgeMetalFrontend.mm stageText):
# the name it was specialized under and "spec", made with no constant values
STAGE_NAMES = re.compile(r'( \| [vf] \S+)(?: as=\S+)?(?: spec(?= ))?')
# The OS builds a corpus line is a state of (tools/gpu/make_pack.py own_build strips it from the lines a pack takes)
OS_BUILD = re.compile(r' os=\S+')


# The Core Animation states the pack holds (ca/caset.py's tiers): every one. The runtime makes a state and places its
# programs in the shader heap at its first lookup (gpu.cpp Device::pipeline), so the pack's size is file size only.
CA_TIERS = ('corpus', 'demand', 'manifest', 'variants', 'blend', 'fetch', 'closure')
# The tiers the generator derives and no device or manifest named: their states are laid after the launch set
# (runtime/pack.h PackHeader::tier0), their lines marked by cold_line
COLD_TIERS = ('variants', 'blend', 'fetch', 'closure')


def cold_line(line: str) -> str:
    """Give a corpus line marked as a state of tier 1 (probe_states.h State::tier); its key is the line's as it was."""
    return line.replace(' samples=', ' tier=1 samples=', 1)


def ca_corpus(qc: Path, corpus: Path, work: Path, names: list[str] = (), preferences: Path = STAGER) -> Path:
    """Give the corpus with its Core Animation lines as harvested and those made from their names (ca/ tables).

    RenderBox's states follow the corpus's own, with the tier-1 closure rb/rbclosure.py makes of them. Without this
    QuartzCore's tables in ca/, the corpus as it is.
    """
    digest = hashlib.sha256(qc.read_bytes()).hexdigest()
    for path in sorted((HERE / 'ca').glob('qc-*.json')):
        with open(path) as f:
            if json.load(f).get('metallib_sha256') == digest:
                break
    else:
        print(f"make_pack: no QuartzCore tables for default.metallib {digest[:16]}: the corpus's states as they are")
        return corpus
    out = work / 'corpus.ca.log'
    with open(corpus) as src:
        lines = src.readlines()
    sys.path.insert(0, str(HERE / 'ca'))
    import caspec  # noqa: PLC0415 (ca/ is only on the path here)
    import deepcolour  # noqa: PLC0415

    cif10 = deepcolour.cif10(preferences)
    print(f'make_pack: CADeviceUseCIF10 as {preferences.name} leaves it: {"MobileGestalt" if cif10 is None else cif10}')
    renderbox, why = renderbox_tagger(digest)
    with open(out, 'w') as dst:
        dst.writelines(renderbox(line) for line in lines if not caspec.corpus_fields(line)[0])
        dst.writelines(cold_line(renderbox(line)) + '\n' for line in renderbox_closure(digest, lines))
        for tier, made in ca_states(path, corpus, names, cif10):
            dst.writelines(line + '\n' for line in made)
            print(f'make_pack: Core Animation states, {tier}: {len(made)}')
    for reason, count in sorted(why.items()):
        print(f'make_pack: RenderBox fetching states, {reason}: {count}')
    return out


def renderbox_tagger(digest: str) -> tuple[Callable[[str], str], dict[str, int]]:
    """Give the function tagging RenderBox's lines with their fetch-free forms (rb/rbfree.py), and its counts.

    For the IPSW whose QuartzCore library has SHA-256 `digest`; the lines as they are where rb/ has no tables for it.
    """
    sys.path.insert(0, str(HERE / 'rb'))
    import rbfree  # noqa: PLC0415 (rb/ is only on the path here)

    made = rbfree.tagger(HERE / 'rb', digest)
    if made is None:
        print(f'make_pack: no RenderBox tables for QuartzCore {digest[:16]}: its states as they are')
        return (lambda line: line), {}
    return made


def renderbox_closure(digest: str, lines: list[str]) -> list[str]:
    """Give the RenderBox states rb/rbclosure.py makes of the corpus's that the corpus lacks.

    For the IPSW whose QuartzCore library has SHA-256 `digest`; none where its rb/ table has no closure.
    """
    sys.path.insert(0, str(HERE / 'rb'))
    import rbclosure  # noqa: PLC0415 (rb/ is only on the path here)

    made = rbclosure.made(HERE / 'rb', digest, lines, state_key)
    print(f'make_pack: RenderBox states, closure: {len(made)}')
    return made


def ca_states(
    tables_path: Path, corpus: Path, names: list[str] = (), cif10: bool | None = None
) -> list[tuple[str, list[str]]]:
    """Give the Core Animation states the pack holds, each once by its key, by tier: those harvested, then caset's.

    Every state a device of this build drew goes in as it drew it (QuartzCore makes some names otherwise than the
    journal holds them, a harvested c1=550 state regenerated with c1=115, or not at all), but one whose function
    constants are not those this QuartzCore makes of its name (caspec.made_of_name), then those made from their names
    (ca/caset.py, ca/caspec.py: QuartzCore's own decoding of a name into a state, with this IPSW's tables,
    ca/qc-<build>.json), every build's journal's names among them.
    """
    sys.path.insert(0, str(HERE / 'ca'))
    import caset  # noqa: PLC0415 (ca/ is only on the path here)
    import caspec  # noqa: PLC0415

    tables = caspec.Tables(str(tables_path))
    keys: set[str] = set()

    def fresh(line: str) -> bool:
        key = state_key(line)
        return not (key in keys or keys.add(key))

    with open(corpus) as f:
        drawn = [line.rstrip('\n') for line in f if line.startswith('R ok=1 ') and caspec.corpus_fields(line)[0]]
    drawn = [line for line in drawn if drawn_here(line, tables.t['build'])]
    foreign = {line for line in drawn if not caspec.made_of_name(tables, line)}
    print(f"make_pack: Core Animation states harvested, another build's constants for their names: {len(foreign)}")
    out = [('harvested', [line for line in drawn if line not in foreign and fresh(line)])]
    named = caset.corpus_names(str(corpus), tables) + [n for n in names if caset.makes(tables, n)]
    for tier, labels in caset.labels(tables, named, CA_TIERS, cif10).items():
        out.append((tier, [line for line in (caspec.state_line(tables, n) for n in labels) if fresh(line)]))
    why: dict[str, int] = {}
    tagged = [
        (tier, [(cold_line if tier in COLD_TIERS else str)(caset.with_fetch_rung(tables, line, why)) for line in lines])
        for tier, lines in out
    ]
    for reason, count in sorted(why.items()):
        print(f'make_pack: fetching states, {reason}: {count}')
    return tagged


def drawn_here(line: str, build: str) -> bool:
    """Whether a corpus line is a state of `build`: one the corpus step took (it strips their tag) or one naming it.

    The step keeps another build's Core Animation lines, tagged, for their names alone.
    """
    tag = OS_BUILD.search(line.split(' | ', 1)[0])
    return not tag or build in tag[0].split('=', 1)[1].split(',')


# The runtime's sources directpack.mm makes its records with (TegraGM20BMetalDirect.mm beside them)
DIRECT_RUNTIME = ('direct', 'direct_compute', 'record_parts', 'emit', 'pack', 'metallib')


class Links(NamedTuple):
    """What the tools link: the translation and the stage compile."""

    translate: Path
    compile_object: Path


def build(work: Path, links: Links, metal_abi: Path, context_header: Path) -> tuple[str, ...]:
    """Build the mock GPU, the pack's tools and the driver's own states into `work`, and run the context init.

    The mock GPU with nvdec on every submission, directpack and native-seed over the translation (beside the mock:
    rpath @executable_path) and the driver's own states: the YCbCr conversions (ycbcr.metal) and the extended-range
    stand-ins (xr.metal, xr_target.metal), an iOS library like QuartzCore's, made after Core Animation's (directpack.mm
    PROBE_OWN_LIB). Gives the tools made (PACK_TOOLS).
    """
    translate, compile_object = links
    # Source paths (Mesa's asserts' __FILE__) as /switch-ios/..., whichever checkout or sandbox this runs in: the tools
    # and the mock are an action's outputs, and the run's cache key is their bytes.
    maps = [f'-ffile-prefix-map={METAL.parents[1]}=/switch-ios/tree', f'-ffile-prefix-map={Path.cwd()}=/switch-ios']
    cflags = ['-O2', '-Wall', *maps, '-I', ABI, '-I', CLASSES]
    objs = []
    for src in ('mesa/mme_fermi.c', 'mesa/mme_fermi_sim.c'):
        obj = work / (Path(src).stem + '.o')
        run(['xcrun', 'clang', '-O2', '-std=c11', '-w', *maps, '-I', HERE / 'mesa', '-c', HERE / src, '-o', obj])
        objs.append(obj)
    run(['xcrun', 'clang', *cflags, '-c', HERE / 'mock_iokit.c', '-o', work / 'mock_iokit.o'])
    run(['xcrun', 'clang++', *cflags, '-std=c++17', '-c', HERE / 'cap.cpp', '-o', work / 'cap.o'])
    nvdec = ('nvdec', 'nvdec_copy', 'nvdec_text', 'nvdec_load', 'nvdec_state', 'nvdec_draw')
    for unit in nvdec:
        run(['xcrun', 'clang++', *cflags, '-std=c++17', '-c', HERE / f'{unit}.cpp', '-o', work / f'{unit}.o'])
    run(['xcrun', 'clang++', *cflags, '-std=c++17', '-c', HERE / 'nvdec_compute.cpp', '-o', work / 'nvdec_compute.o'])
    run(
        [
            'xcrun',
            'clang++',
            '-dynamiclib',
            work / 'mock_iokit.o',
            work / 'cap.o',
            *(work / f'{unit}.o' for unit in nvdec),
            work / 'nvdec_compute.o',
            *objs,
            '-framework',
            'IOKit',
            '-framework',
            'CoreFoundation',
            '-install_name',
            '@rpath/libmock_iokit.dylib',
            '-o',
            work / 'libmock_iokit.dylib',
        ]
    )
    run(
        [
            'xcrun',
            'clang++',
            *cflags,
            '-std=c++17',
            '-I',
            context_header.parent,
            HERE / 'context_state.cpp',
            '-L',
            work,
            '-lmock_iokit',
            '-Wl,-rpath,@executable_path',
            '-o',
            work / 'context-state',
        ]
    )
    run([work / 'context-state'], out=work / 'context.log')

    def tool(name: str, objcxx: list[Path], cxx: tuple[Path, ...] = ()) -> None:
        """Link one of the generator's tools over the translation, beside the mock GPU (rpath).

        A tool of the direct compile (direct.cpp among `cxx`) links the stage compile's object.
        """
        compiles = any(p.name == 'direct.cpp' for p in cxx)
        frameworks = [
            f
            for framework in ('Foundation', 'Metal', 'IOSurface', 'IOKit', 'CoreVideo')
            for f in ('-framework', framework)
        ]
        run(
            [
                'xcrun',
                'clang++',
                '-O2',
                '-std=c++17',
                '-fobjc-arc',
                '-Wno-deprecated-declarations',
                *maps,
                '-I',
                PLUGIN,
                '-I',
                metal_abi.parent,
                '-I',
                ABI,
                '-I',
                CLASSES,
                '-x',
                'objective-c++',
                *objcxx,
                *(['-x', 'c++', *cxx] if cxx else []),
                '-x',
                'none',
                translate,
                *([compile_object] if compiles else []),
                '-L',
                work,
                '-lmock_iokit',
                '-Wl,-rpath,@executable_path',
                # No debug map: its paths name the build's sandbox or worker slot, so every link made other bytes and
                # every bucket keyed on the tool ran again
                '-Wl,-S',
                '-lobjc',
                *frameworks,
                '-o',
                work / name,
            ]
        )

    tool(
        'directpack',
        [
            HERE / 'directpack.mm',
            METAL / 'TegraGM20BMetalOffline.mm',
            METAL / 'TegraGM20BMetalOfflineReflection.mm',
            METAL / 'TegraGM20BMetalDirect.mm',
            METAL / 'TegraGM20BMetalRenderKey.mm',
        ],
        tuple(METAL / f'{name}.cpp' for name in DIRECT_RUNTIME),
    )
    tool(
        'native-seed',
        [
            HERE / 'native_seed.mm',
            METAL / 'TegraGM20BMetalOffline.mm',
            METAL / 'TegraGM20BMetalDirect.mm',
            METAL / 'TegraGM20BMetalRenderKey.mm',
        ],
        tuple(METAL / f'{name}.cpp' for name in (*DIRECT_RUNTIME, 'gm20b_cache', 'kernel_cache')),
    )
    own = ('ycbcr', 'ycbcr_store', 'xr', 'xr_target', 'resolve', 'mip', 'turn', 'stencil_bits', 'fmt', 'decode')
    # resolve.metal's sums in sample order and its NaN tests kept; ycbcr_store.metal's NaN tests and half rounding;
    # stencil_bits.metal's depth comparisons; fmt.metal's NaN tests and half rounding; xr_target.metal's NaN tests and
    # its stores' comparisons with the tile; decode.metal's integer decodes, which no fast-math rewrite may touch
    exact = {
        'resolve': ['-fno-fast-math'],
        'ycbcr_store': ['-fno-fast-math'],
        'stencil_bits': ['-fno-fast-math'],
        'fmt': ['-fno-fast-math'],
        'xr_target': ['-fno-fast-math'],
        'decode': ['-fno-fast-math'],
    }
    for name in own:
        run(
            [
                'xcrun',
                '-sdk',
                'iphoneos',
                'metal',
                '-c',
                HERE / f'{name}.metal',
                '-I',
                HERE,
                *exact.get(name, []),
                '-o',
                work / f'{name}.air',
            ]
        )
    run(['xcrun', '-sdk', 'iphoneos', 'metallib', *(work / f'{name}.air' for name in own), '-o', work / 'own.metallib'])
    return PACK_TOOLS


# The states are probed in buckets, each its own probe processes over a share of the states (fnv64 of the state's line,
# so a state stays in its bucket whatever else changes), all at once: a bucket's outputs are a function of its states
# and the tools alone (what a cache of probe runs can key by). Bucket 0 also draws the driver's own states
# (PROBE_OWN_LIB). Our own apps' states (APP_LIBRARY) are one more bucket after these, the only one whose probe reads
# the apps' libraries: an app's shader edit reruns that bucket alone.
BUCKETS = 16
APP_LIBRARY = re.compile(r' lib=/Applications/Switch\w*\.app/default\.metallib')


def split_buckets(corpus: Path, work: Path, count: int) -> list[Path]:
    """Write each bucket's corpus (its Core Animation lines, in corpus order) into work/bucket<i>/corpus.log.

    The count buckets share every state but our own apps', which bucket<count> holds.

    Each bucket starts with the first downsample_4_frag state: the probe's warm-up draws it so its dummy sampler takes
    NVK's sampler index 0 (layouts.py reads a handle naming index 0 as no sampler). A bucket that also holds it probes
    it twice; mkpack.py keeps one.
    """
    lines: list[list[str]] = [[] for _ in range(count + 1)]
    warm = ''
    with open(corpus) as f:
        for line in f:
            if line.startswith('R ') and (' label=com.apple.coreanimation.' in line or FRAMEWORK.search(line)):
                at = count if APP_LIBRARY.search(line) else packfile.fnv(line.split(' ', 3)[3].encode()) % count
                lines[at].append(line)
                warm = warm or (line if 'f downsample_4_frag' in line else '')
            elif line.startswith('D ok=1 '):  # a compute dispatch shape (directpack.mm's kernels)
                lines[packfile.fnv(line.encode()) % count].append(line)
    dirs = []
    for i, part in enumerate(lines):
        d = work / f'bucket{i}'
        d.mkdir(parents=True)
        (d / 'corpus.log').write_text(warm + ''.join(part))
        dirs.append(d)
    return dirs


def state_key(line: str) -> str:
    """Give an R line's state key as its text names it: every descriptor field but the label, then its functions.

    The pack files a state under the key the runtime computes of the descriptor made of the line (probe_states.h,
    runtime/TegraGM20BMetalPipelineKey.h), which is this for a line written as the device journals it.

    No lib= field is in it, nor the journal's spec mark, nor a stage's as= specialized name: the runtime keys a stage by
    its library function and constants alone (TegraGM20BMetalLibrary.mm metal_pipeline_key,
    TegraGM20BMetalCompileRequest.mm requestPackKey, probe_states.h parseState). The functions the state links follow
    its stages, each with as=<the name it is specialized under> where that is not its library function's
    (TegraGM20BMetalPipelineKey.h linkedSegment); a line whose first library is another framework's default.metallib
    than QuartzCore's ends with " | l <framework>" (probe.mm libraryPath, frameworkName).
    """
    libs = LIBRARY.findall(LINKED_LIBRARY.sub(r'\1', line))
    framework = ''
    if libs and libs[0].endswith('.framework/default.metallib') and '/QuartzCore.framework/' not in libs[0]:
        framework = libs[0].rsplit('.framework/', 1)[0].rsplit('/', 1)[-1]
    line = LIBRARY.sub('', line)
    bar1, bar2 = line.find(' | v '), line.find(' | f ')
    if bar1 < 0 or bar2 < 0:
        return ''
    bar3 = line.find(' | ', bar2 + 5)
    fields = [
        h
        for h in line[:bar1].split()
        if h.startswith(('samples=', 'topo=', 'ds=', 'alphacov=')) or (len(h) > 1 and h[0] in 'ca' and h[1].isdigit())
    ] + [h for h in line[:bar1].split() if h == 'rast=0']
    key = ' '.join(fields) + ' ' + STAGE_NAMES.sub(r'\1', line[bar1 : bar3 if bar3 >= 0 else len(line)])
    cubic = sampled = written = ''
    for segment in line[bar3 + 3 :].split(' | ') if bar3 >= 0 else ():
        if segment.startswith(LINKED):
            tokens = segment.split()
            named = [t for t in tokens[2:] if t.startswith('as=')]
            values = [t for t in tokens[2:] if not t.startswith('as=') and t != 'spec']
            key += ' | ' + ' '.join(tokens[:2] + named) + ' ' + ' '.join(values)
        elif segment.startswith('cubic '):
            cubic = ' | ' + ' '.join(segment.split())
        elif segment.startswith('xrs='):
            sampled = ' | ' + segment.strip()
        elif segment.startswith('wtex='):
            written = ' | ' + segment.strip()
    # A stage with no constants keeps its space before what follows it (TegraGM20BMetalPipelineKey.h pipelineKey trims
    # the key's end alone): " | f <function>  | l <framework>"
    return (f'{key} | l {framework}' if framework else key).strip() + cubic + sampled + written


def stamp(pack_bin: Path, stack: int) -> None:
    """Write the stack key into pack.bin's header."""
    pack = bytearray(pack_bin.read_bytes())
    # PackHeader.stack, after the section table
    struct.pack_into('<Q', pack, 8 + 16 + 8 * 2 * packfile.SECTIONS, stack)
    pack_bin.write_bytes(pack)


def link(out: Path, buckets: list[Path], stack: int, inventory: Path | None = None) -> None:
    """directlink.py over the buckets' records: out/pack.bin, stamped with the stack key.

    `inventory`: the IPSW's archive inventory, whose archives' declared states the pack lists (archives.py).
    """
    out.mkdir(parents=True, exist_ok=True)
    archives = ['--archives', inventory] if inventory else []
    report = run([sys.executable, HERE / 'directlink.py', out, *archives, *buckets])
    if stack:
        stamp(out / 'pack.bin', stack)
    print(report.rstrip())


def stamp_or_link(stack: int) -> bool:
    """Run the --stamp or --link step where the command line names one; return whether it did."""
    if '--stamp' in sys.argv[1:]:  # --stamp <pack.bin> --stack <key>
        stamp(Path(option('--stamp')), stack)
        return True
    if '--link' in sys.argv[1:]:  # --link --out <dir> [--stack <key>] [--archives <inventory.tsv>] <bucket dir>...
        out = Path(option('--out')).resolve()
        buckets = [Path(a).resolve() for a in sys.argv[sys.argv.index('--link') + 1 :] if Path(a).is_dir()]
        buckets = [b for b in buckets if b != out]
        inventory = option('--archives', False)
        link(out, buckets, stack, Path(inventory).resolve() if inventory else None)
        return True
    return False


def main() -> None:
    if '--build-only' in sys.argv[1:]:
        out = Path(option('--out')).resolve()
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory() as scratch:
            made = build(
                Path(scratch),
                Links(
                    Path(option('--translate')).resolve(),
                    Path(option('--compile')).resolve(),
                ),
                Path(option('--metal-abi')).resolve(),
                Path(option('--context-header')).resolve(),
            )
            for name in made:
                shutil.copyfile(Path(scratch) / name, out / name)
                (out / name).chmod(0o755)
        return
    stack = int(option('--stack', False) or '0', 16)
    if stamp_or_link(stack):
        return
    qc = Path(option('--qc')).resolve()
    corpus = Path(option('--corpus')).resolve()
    out = Path(option('--out')).resolve()
    work = out / 'work'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    names = Path(option('--ca-names', False) or os.devnull).read_text().split()
    corpus = ca_corpus(qc, corpus, work, names, Path(option('--ca-preferences', False) or STAGER).resolve())
    if '--split' in sys.argv[1:]:  # --split --qc <metallib> --corpus <corpus.log> --out <dir>: <dir>/work/bucket<i>
        split_buckets(corpus, work, BUCKETS)
        return
    prebuilt = option('--prebuilt', False)
    if prebuilt:
        for name in PACK_TOOLS:
            shutil.copyfile(Path(prebuilt) / name, work / name)
            (work / name).chmod(0o755)
    else:
        build(
            work,
            Links(
                Path(option('--translate')).resolve(),
                Path(option('--compile')).resolve(),
            ),
            Path(option('--metal-abi')).resolve(),
            Path(option('--context-header')).resolve(),
        )
    from directbucket import bucket, probe_env  # noqa: PLC0415 (no other step reads it: the build keys none of them on it)

    env = probe_env()
    buckets = split_buckets(corpus, work, BUCKETS)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
        for line in pool.map(lambda b: bucket(work, b, qc, env), buckets):
            print(line)
    link(out, buckets, stack)
    if '--clean' in sys.argv[1:]:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
