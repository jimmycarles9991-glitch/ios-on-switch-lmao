#!/usr/bin/env python3
"""rawpack: the raw GM20B path's pipeline pack (runtime/raw_pack.h), made on this Mac.

Core Animation's render pipeline states (PROBE_LABEL: the render server's) are made as the runtime makes a state the
pack lacks: this build's own frontend + m2v (the host bridge) translate each, and NVK (the host twin with the device's
pipeline cache identity) compiles its stages, with no pipeline, draw or capture (directpack.mm, runtime/RawDirect.h).
The harvested Core Animation lines go in as drawn, beside the states made from their names (ca/caset.py, ca/caspec.py:
QuartzCore's own decoding of a name into a state, with this IPSW's tables, ca/qc-<build>.json): QuartzCore's offline
compilation manifest, and the closure over the blend CA puts in the pipeline and over the blend it leaves in the
fragment (CA_TIERS).

  1. directpack.mm makes every state's record: its programs, the 3D registers raw_emit makes for it, its push words and
     its set-0 layout; qualified ordered framebuffer-fetch records retain their barrier twins; and every compute
     dispatch shape's kernel (runtime/raw_direct_compute.h);
  2. context_state.cpp runs the channel's context init (kits/SwitchGM20BEmit's) through nvdec (pack/nvdec.cpp, Mesa's
     Fermi MME simulator in pack/mesa): the registers every record is laid over, with draw_context.txt;
  3. directlink.py writes pack.bin by mkpack.py's rules: per state its programs, registers, push words and layout, plus
     the context init with the addresses to relocate.

usage: rawpack.py --bridge <host libnative_metal_bridge.a> --metal-abi <MetalABI.h> --nvk <host twin dylib>
                  --qc <default.metallib> --corpus <corpus.log> --out <dir> [--stack <hex key>] [--prebuilt <dir>]
       rawpack.py --build-only --bridge <host libnative_metal_bridge.a> --metal-abi <MetalABI.h>
                  --context-header <SwitchGM20BContext.h> --out <dir>
writes <out>/pack.bin, pack.json, layouts.txt, report.txt and keeps the buckets in <out>/work (deleted with --clean).
--build-only compiles directpack, the probe, the mock GPU (libmock_iokit.dylib) and own.metallib into <dir>, with the
channel's context init as nvdec runs it (context.log), and stops;
--prebuilt <dir> runs those instead of compiling them (//gpu/seed:local_tools makes them its own action, so a corpus
edit only reruns the buckets).
--ca-names: Core Animation states a device's logs name as missing from the pack (precompile/ca_names.py), made from
their names in the corpus tier, as the journal's own are.
--ca-preferences: the GPU stager (stage-tegra-gm20b-metal.py, the default) whose Core Animation preferences decide the
colour precision the device's QuartzCore names its states with (ca/deepcolour.py).
--stack: the build's stack key (the plugin is compiled with the same value), stamped into the pack header: the runtime
refuses a pack of another build.
The same work in three steps, for a build that caches each bucket's run (it depends on the bucket's state lines and the
tools alone):
       rawpack.py --split --qc <default.metallib> --corpus <corpus.log> [--ca-names <ca-names/<build>.txt>]
                  [--ca-preferences <stager>] --out <dir>
           writes <dir>/work/bucket0 .. bucket15, each a corpus.log of its states
       rawpack.py --probe-bucket <bucket dir> --prebuilt <dir> --nvk <host twin dylib> --qc <default.metallib>
                  [--root <dir>]
           makes one bucket in place: records.bin (its states' and kernels' records), and bucket 0's context.log;
           a line's lib= path is under --root (the crawl's root/ with the build's applications), else QuartzCore's
       rawpack.py --link --out <dir> [--stack <hex key>] [--archives <inventory.tsv>] <bucket dir>...
           directlink.py over the buckets: <dir>/pack.bin and the rest, as the one-step form writes them, with the
           archive inventory's declared states (archives.py)
       rawpack.py --stamp <pack.bin> --stack <hex key>
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
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packfile

HERE = Path(__file__).resolve().parent  # drivers/TegraGM20BMetal/raw/pack
RAW = HERE.parent
TREE = Path(os.environ.get('RAWPACK_TREE', RAW.parents[2]))  # tree/ (drivers/TegraGM20BMetal/raw -> tree)
METAL = TREE / 'drivers/NativeMetal'
ABI = TREE / 'drivers/TegraGM20B'
CLASSES = TREE / 'third_party/mesa/src/nouveau/headers'  # NVIDIA's class headers find nvtypes.h here (nvdec.cpp)
STAGER = RAW.parent / 'stage-tegra-gm20b-metal.py'
# Lines the bridge's direct compile writes to stderr (NativeMetalDirect.rs translate): the build's output carries them
DIRECT_COMPILE_SAYS = 'direct compile: '


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
            f'rawpack: FAILED: {" ".join(map(str, cmd))[:400]}\n{(r.stdout or "")[-3000:]}{(r.stderr or "")[-5000:]}'
        )
    return (r.stdout or '') + (r.stderr or '')


def option(name: str, required: bool = True) -> str | None:
    if name in sys.argv[1:-1]:
        return sys.argv[sys.argv.index(name) + 1]
    if required:
        sys.exit(__doc__)
    return None


BUILT = ('probe', 'directpack', 'native-seed', 'compile-profile', 'libmock_iokit.dylib', 'own.metallib', 'context.log')
# Another framework's states, a bundle's or a framework resource library's, and our own apps' (their lines keep lib=,
# tools/gpu/rawpack.py framework_line and app_line): the probe loads that library from the crawl's root
FRAMEWORK = re.compile(
    r' lib=(?:/System/Library/(?:(?:Private)?Frameworks/\w+\.framework/default'
    r'|\S+?\.(?:bundle|framework/Resources)/\S+)|/Applications/Switch\w*\.app/default)\.metallib '
)
QC_PATH = 'System/Library/Frameworks/QuartzCore.framework/default.metallib'
LIBRARY = re.compile(r' lib=(\S+)')
# A function a state links (" | <vl|vp|fl|fp> <function> lib=<library> ..."): its library is not the state's
LINKED_LIBRARY = re.compile(r'( \| [vf][lp] \S+) lib=\S+')
LINKED = ('vl ', 'vp ', 'fl ', 'fp ')
# The OS builds a corpus line is a state of (tools/gpu/rawpack.py own_build strips it from the lines a pack takes)
OS_BUILD = re.compile(r' os=\S+')


def library_root(qc: Path) -> str:
    """Give the crawl root that QuartzCore's default.metallib lies in.

    Other frameworks' libraries are under the same root.
    """
    return str(qc)[: -len(QC_PATH) - 1] if str(qc).endswith(QC_PATH) else ''


# The Core Animation states the pack holds (ca/caset.py's tiers): every one. The runtime makes a state and places its
# programs in the shader heap at its first lookup (raw_gpu.cpp Device::pipeline), so the pack's size is file size only.
CA_TIERS = ('corpus', 'manifest', 'variants', 'blend', 'fetch')


def ca_corpus(qc: Path, corpus: Path, work: Path, names: list[str] = (), preferences: Path = STAGER) -> Path:
    """Give the corpus with its Core Animation lines as harvested and those made from their names (ca/ tables).

    Without this QuartzCore's tables in ca/, the corpus as it is.
    """
    digest = hashlib.sha256(qc.read_bytes()).hexdigest()
    for path in sorted((HERE / 'ca').glob('qc-*.json')):
        with open(path) as f:
            if json.load(f).get('metallib_sha256') == digest:
                break
    else:
        print(f"rawpack: no QuartzCore tables for default.metallib {digest[:16]}: the corpus's states as they are")
        return corpus
    out = work / 'corpus.ca.log'
    with open(corpus) as src:
        lines = src.readlines()
    sys.path.insert(0, str(HERE / 'ca'))
    import caspec  # noqa: PLC0415 (ca/ is only on the path here)
    import deepcolour  # noqa: PLC0415

    cif10 = deepcolour.cif10(preferences)
    print(f'rawpack: CADeviceUseCIF10 as {preferences.name} leaves it: {"MobileGestalt" if cif10 is None else cif10}')
    with open(out, 'w') as dst:
        dst.writelines(line for line in lines if not caspec.corpus_fields(line)[0])
        for tier, made in ca_states(path, corpus, names, cif10):
            dst.writelines(line + '\n' for line in made)
            print(f'rawpack: Core Animation states, {tier}: {len(made)}')
    return out


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
    print(f"rawpack: Core Animation states harvested, another build's constants for their names: {len(foreign)}")
    out = [('harvested', [line for line in drawn if line not in foreign and fresh(line)])]
    named = caset.corpus_names(str(corpus), tables) + [n for n in names if caset.makes(tables, n)]
    for tier, labels in caset.labels(tables, named, CA_TIERS, cif10).items():
        out.append((tier, [line for line in (caspec.state_line(tables, n) for n in labels) if fresh(line)]))
    why: dict[str, int] = {}
    tagged = [(tier, [caset.with_fetch_rung(tables, line, why) for line in lines]) for tier, lines in out]
    for reason, count in sorted(why.items()):
        print(f'rawpack: fetching states, {reason}: {count}')
    return tagged


def drawn_here(line: str, build: str) -> bool:
    """Whether a corpus line is a state of `build`: one the corpus step took (it strips their tag) or one naming it.

    The step keeps another build's Core Animation lines, tagged, for their names alone.
    """
    tag = OS_BUILD.search(line.split(' | ', 1)[0])
    return not tag or build in tag[0].split('=', 1)[1].split(',')


# The raw runtime's sources directpack.mm makes its records with (RawDirect.mm beside them)
DIRECT_RUNTIME = ('raw_direct', 'raw_direct_compute', 'raw_record_parts', 'raw_emit', 'raw_pack', 'raw_metallib')
# The frontend's exports raw refers to weakly (native_bridge_metal_<name>), unbound in directpack
FRONTEND_WEAK = ('render_direct', 'compute_direct', 'nvk_symbol', 'function_metallib', 'function_library_name')


def build(work: Path, bridge: Path, metal_abi: Path, context_header: Path) -> None:
    """Build the mock GPU, the probe and the raw path's own states into `work`, and run the context init.

    The mock GPU with nvdec on every submission, the probe through the frontend + host bridge (beside it: rpath
    @executable_path) and the raw path's own states: the YCbCr conversions (ycbcr.metal) and the extended-range
    stand-ins (xr.metal), an iOS library like QuartzCore's, drawn after Core Animation's (probe.mm PROBE_OWN_LIB).
    """
    # Source paths (Mesa's asserts' __FILE__) as /switch-ios/..., whichever checkout or sandbox this runs in: the probe
    # and the mock are an action's outputs, and the run's cache key is their bytes.
    maps = [f'-ffile-prefix-map={RAW.parents[2]}=/switch-ios/tree', f'-ffile-prefix-map={Path.cwd()}=/switch-ios']
    cflags = ['-O2', '-Wall', *maps, '-I', ABI, '-I', CLASSES]
    objs = []
    for src in ('mesa/mme_fermi.c', 'mesa/mme_fermi_sim.c'):
        obj = work / (Path(src).stem + '.o')
        run(['xcrun', 'clang', '-O2', '-std=c11', '-w', *maps, '-I', HERE / 'mesa', '-c', HERE / src, '-o', obj])
        objs.append(obj)
    run(['xcrun', 'clang', *cflags, '-c', HERE / 'mock_iokit.c', '-o', work / 'mock_iokit.o'])
    run(['xcrun', 'clang++', *cflags, '-std=c++17', '-c', HERE / 'cap.cpp', '-o', work / 'cap.o'])
    run(['xcrun', 'clang++', *cflags, '-std=c++17', '-c', HERE / 'nvdec.cpp', '-o', work / 'nvdec.o'])
    run(['xcrun', 'clang++', *cflags, '-std=c++17', '-c', HERE / 'nvdec_compute.cpp', '-o', work / 'nvdec_compute.o'])
    run(
        [
            'xcrun',
            'clang++',
            '-dynamiclib',
            work / 'mock_iokit.o',
            work / 'cap.o',
            work / 'nvdec.o',
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

    def tool(name: str, objcxx: list[Path], cxx: tuple[Path, ...] = (), frontend: bool = True) -> None:
        """Link one of the generator's tools over the host bridge (and the frontend), beside the mock GPU (rpath).

        Without the frontend, raw's weak references to it (RawDirect.mm, RawRenderKey.mm) stay unbound.
        """
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
                METAL,
                '-I',
                metal_abi.parent,
                '-I',
                ABI,
                '-I',
                CLASSES,
                '-x',
                'objective-c++',
                *objcxx,
                *([METAL / 'NativeBridgeMetalFrontend.mm'] if frontend else []),
                *(['-x', 'c++', *cxx] if cxx else []),
                '-x',
                'none',
                bridge,
                '-L',
                work,
                '-lmock_iokit',
                '-Wl,-rpath,@executable_path',
                *([] if frontend else [f'-Wl,-U,_native_bridge_metal_{weak}' for weak in FRONTEND_WEAK]),
                '-lobjc',
                *frameworks,
                '-o',
                work / name,
            ]
        )

    tool('probe', [HERE / 'probe.mm'])
    tool(
        'directpack',
        [
            HERE / 'directpack.mm',
            HERE / 'offline_metal.mm',
            RAW / 'runtime/RawDirect.mm',
            RAW / 'runtime/RawRenderKey.mm',
        ],
        tuple(RAW / f'runtime/{name}.cpp' for name in DIRECT_RUNTIME),
        frontend=False,
    )
    tool('compile-profile', [HERE / 'compile_profile.mm'])
    tool(
        'native-seed',
        [HERE / 'native_seed.mm', RAW / 'runtime/RawDirect.mm', RAW / 'runtime/RawRenderKey.mm'],
        tuple(RAW / f'runtime/{name}.cpp' for name in (*DIRECT_RUNTIME, 'raw_cache', 'raw_kernel_cache')),
    )
    own = ('ycbcr', 'ycbcr_store', 'xr', 'resolve', 'mip', 'turn', 'stencil_bits')
    # resolve.metal's sums in sample order and its NaN tests kept; ycbcr_store.metal's NaN tests and half rounding;
    # stencil_bits.metal's depth comparisons
    exact = {'resolve': ['-fno-fast-math'], 'ycbcr_store': ['-fno-fast-math'], 'stencil_bits': ['-fno-fast-math']}
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


# The states are probed in buckets, each its own probe processes over a share of the states (fnv64 of the state's line,
# so a state stays in its bucket whatever else changes), all at once: a bucket's outputs are a function of its states
# and the tools alone (what a cache of probe runs can key by). Bucket 0 also draws the raw path's own states
# (PROBE_OWN_LIB).
BUCKETS = 16


def split_buckets(corpus: Path, work: Path, count: int) -> list[Path]:
    """Write each bucket's corpus (its Core Animation lines, in corpus order) into work/bucket<i>/corpus.log.

    Each bucket starts with the first downsample_4_frag state: the probe's warm-up draws it so its dummy sampler takes
    NVK's sampler index 0 (layouts.py reads a handle naming index 0 as no sampler). A bucket that also holds it probes
    it twice; mkpack.py keeps one.
    """
    lines: list[list[str]] = [[] for _ in range(count)]
    warm = ''
    with open(corpus) as f:
        for line in f:
            if line.startswith('R ') and (' label=com.apple.coreanimation.' in line or FRAMEWORK.search(line)):
                lines[packfile.fnv(line.split(' ', 3)[3].encode()) % count].append(line)
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
    runtime/RawPipelineKey.h), which is this for a line written as the device journals it.

    No lib= field is in it; the functions the state links follow its stages (probe_states.h linkedFunction); a line
    whose first library is another framework's default.metallib than QuartzCore's ends with " | l <framework>"
    (probe.mm libraryPath, frameworkName).
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
        if h.startswith(('samples=', 'topo=', 'ds=')) or (len(h) > 1 and h[0] in 'ca' and h[1].isdigit())
    ]
    key = ' '.join(fields) + ' ' + line[bar1 : bar3 if bar3 >= 0 else len(line)]
    cubic = sampled = written = ''
    for segment in line[bar3 + 3 :].split(' | ') if bar3 >= 0 else ():
        if segment.startswith(LINKED):
            tokens = segment.split()
            named = [t for t in tokens[2:] if t.startswith('as=')]
            values = [t for t in tokens[2:] if not t.startswith('as=')]
            key += ' | ' + ' '.join(tokens[:2] + named) + ' ' + ' '.join(values)
        elif segment.startswith('cubic '):
            cubic = ' | ' + ' '.join(segment.split())
        elif segment.startswith('xrs='):
            sampled = ' | ' + segment.strip()
        elif segment.startswith('wtex='):
            written = ' | ' + segment.strip()
    # A stage with no constants keeps its space before what follows it (RawPipelineKey.h pipelineKey trims the key's end
    # alone): " | f <function>  | l <framework>"
    return (f'{key} | l {framework}' if framework else key).strip() + cubic + sampled + written


def bucket(tools: Path, d: Path, nvk: Path, qc: Path, env: dict[str, str]) -> str:
    """Make one bucket's records directly (directpack.mm: records.bin, its states' and its dispatch shapes' kernels).

    Bucket 0 holds the tools' context.log: the context init and the registers every record is laid over (directlink.py).
    """
    root = env.get('PROBE_LIB_ROOT') or library_root(qc)
    env = dict(env, PROBE_LIB_ROOT=root, PROBE_DATA_LIBS=env.get('PROBE_DATA_LIBS', str(Path(root).parent / 'data')))
    corpus = d / 'corpus.log'
    cache = d / 'cache'
    said = []
    bridge_said = []

    def step(name: str, cmd: list, extra: dict[str, str], out: Path) -> None:
        shutil.rmtree(cache, ignore_errors=True)
        cache.mkdir()
        with open(out, 'w') as stdout:
            r = subprocess.run(
                [str(c) for c in cmd],
                env=dict(env, **extra),
                stdout=stdout,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
        if r.returncode:
            sys.exit(f'rawpack: {d.name} {name} failed:\n{r.stderr[-4000:]}')
        tail = [line for line in r.stderr.splitlines() if line.startswith(name.split(maxsplit=1)[0] + ':')]
        said.append(f'{name}: {tail[-1] if tail else "?"}')
        bridge_said.extend(line for line in r.stderr.splitlines() if line.startswith(DIRECT_COMPILE_SAYS))

    own = {'PROBE_OWN_LIB': str(tools / 'own.metallib')} if d.name == 'bucket0' else {}
    step('directpack', [tools / 'directpack', nvk, qc, corpus, d / 'records.bin'], own, d / 'directpack.out')
    if d.name == 'bucket0':
        shutil.copyfile(tools / 'context.log', d / 'context.log')
    shutil.rmtree(cache, ignore_errors=True)
    return '\n'.join([f'rawpack: {d.name}: ' + '; '.join(said), *bridge_said])


def probe_env() -> dict[str, str]:
    """Give the probe's environment: this process's, less every knob of the stack, plus the probe's own.

    PROBE_LIB_ROOT is --root where the command line names one.
    """
    env = {
        k: v
        for k, v in os.environ.items()
        if not k.startswith(
            ('NAK_', 'NVK_', 'MESA_', 'NMB_', 'M2V_', 'METAL2VULKAN_', 'TEGRA_', 'MTL_', 'NVDEC_', 'RAWCAP_', 'PROBE_')
        )
    }
    # NMB_CACHE_MB: the m2v cache directory holds every state's NVK driver record until mkprograms reads them (the
    # bridge's 256 MiB cap evicts the oldest past ~7000 states, which then bind programs no record names)
    env.update(
        MESA_SHADER_CACHE_DISABLE='true',
        NVDEC_PROBE='1',
        NVDEC_SCRUB='1',
        NVDEC_TRACE='3',
        NMB_ASYNC_PSO='0',
        NMB_ASYNC_PSO_API='0',
        NMB_CACHE_MB='65536',
    )
    if option('--root', False):
        env['PROBE_LIB_ROOT'] = str(Path(option('--root')).resolve())
    return env


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
            build(
                Path(scratch),
                Path(option('--bridge')).resolve(),
                Path(option('--metal-abi')).resolve(),
                Path(option('--context-header')).resolve(),
            )
            for name in BUILT:
                shutil.copyfile(Path(scratch) / name, out / name)
                (out / name).chmod(0o755)
        return
    stack = int(option('--stack', False) or '0', 16)
    if stamp_or_link(stack):
        return
    qc = Path(option('--qc')).resolve()
    nvk = Path(option('--nvk')).resolve() if '--split' not in sys.argv[1:] else qc
    if '--probe-bucket' in sys.argv[1:]:  # --probe-bucket <dir> --prebuilt <tools> --nvk <dylib> --qc <metallib>
        print(
            bucket(Path(option('--prebuilt')).resolve(), Path(option('--probe-bucket')).resolve(), nvk, qc, probe_env())
        )
        return
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
        for name in BUILT:
            shutil.copyfile(Path(prebuilt) / name, work / name)
            (work / name).chmod(0o755)
    else:
        build(
            work,
            Path(option('--bridge')).resolve(),
            Path(option('--metal-abi')).resolve(),
            Path(option('--context-header')).resolve(),
        )
    env = probe_env()
    buckets = split_buckets(corpus, work, BUCKETS)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
        for line in pool.map(lambda b: bucket(work, b, nvk, qc, env), buckets):
            print(line)
    link(out, buckets, stack)
    if '--clean' in sys.argv[1:]:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
