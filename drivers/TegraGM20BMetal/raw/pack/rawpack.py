#!/usr/bin/env python3
"""rawpack: the raw GM20B path's pipeline pack (runtime/raw_pack.h), made on this Mac.

Core Animation's render pipeline states (PROBE_LABEL: the render server's) go through this build's own frontend + m2v
(the host bridge) and NVK (the host twin with the device's pipeline cache identity), over a mock GM20B whose every
submission is executed by nvdec (pack/nvdec.cpp, Mesa's Fermi MME simulator in pack/mesa). The states are made from
their names (ca/caset.py, ca/caspec.py: QuartzCore's own decoding of a name into a state, with this IPSW's tables,
ca/qc-<build>.json): the corpus's names, QuartzCore's offline compilation manifest, their closure over the blend CA
puts in the pipeline and over the blend it leaves in the fragment (CA_TIERS). With no tables of this QuartzCore (another
IPSW's), the corpus's states as they are.

  1. probe (pack/probe.mm) makes every state and draws it once with tagged inputs, four times: payloads of 256, 16 and
     4096 bytes (a uniform buffer's range is NVK's 256-byte rounding of the payload; the 4096-byte run gives the
     function's window), and of 256 bytes bound at an offset of each slot's own (the push word of its residual);
  2. mkprograms.py reads NAK's programs out of the NVK driver records the run left in its m2v cache directory;
  3. mkpack.py reads the probe logs and writes pack.bin: per state its programs, registers, push words and descriptor
     layout, plus NVK's context init with the addresses to relocate. States m2v draws with more than one draw or with
     dispatches are left out: the raw path hands them to m2v. (An extended-range attachment is loaded and stored
     DontCare, so its state's draw is the pass's only one: probe.mm.)

usage: rawpack.py --bridge <host libnative_metal_bridge.a> --metal-abi <MetalABI.h> --nvk <host twin dylib>
                  --qc <default.metallib> --corpus <corpus.log> --out <dir> [--stack <hex key>] [--prebuilt <dir>]
       rawpack.py --build-only --bridge <host libnative_metal_bridge.a> --metal-abi <MetalABI.h> --out <dir>
writes <out>/pack.bin, pack.json, layouts.txt, report.txt and keeps the probe logs in <out>/work (deleted with --clean).
--build-only compiles the probe, the mock GPU (libmock_iokit.dylib) and own.metallib into <dir> and stops; --prebuilt
<dir> runs those instead of compiling them (//gpu/seed:local_tools makes them its own action, so a corpus edit only
reruns the probes).
--stack: the build's stack key (the plugin is compiled with the same value), stamped into the pack header: the runtime
refuses a pack of another build.
A bucket's states that its run drew in metal2vulkan's storage form (the pixel interlock on: framebuffer fetch, whatever
library) are probed once more in <bucket>/barrier with NVK's interlock hidden (NVK_DEBUG=no_fsi, the macOS twin's
alone), so the pack holds their barrier form too (input attachments, barriers between draws).
The same work in three steps, for a build that caches each bucket's probe runs (they depend on the bucket's state lines
and the tools alone):
       rawpack.py --split --qc <default.metallib> --corpus <corpus.log> --out <dir>
           writes <dir>/work/bucket0 .. bucket15, each a corpus.log of its states
       rawpack.py --probe-bucket <bucket dir> --prebuilt <dir> --nvk <host twin dylib> --qc <default.metallib>
           probes one bucket in place: programs.bin, probe{256,16,4096}.{out,log}, and its barrier/ the same
       rawpack.py --link --out <dir> [--stack <hex key>] <bucket dir>...
           mkpack.py over the buckets: <dir>/pack.bin and the rest, as the one-step form writes them
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


BUILT = ('probe', 'libmock_iokit.dylib', 'own.metallib')
# Another framework's states (their lines keep lib=, tools/gpu/rawpack.py): the probe loads that library from the
# crawl's root
FRAMEWORK = re.compile(r' lib=/System/Library/(?:Private)?Frameworks/\w+\.framework/default\.metallib ')
QC_PATH = 'System/Library/Frameworks/QuartzCore.framework/default.metallib'
LIBRARY = re.compile(r' lib=(\S+)')


def library_root(qc: Path) -> str:
    """Give the crawl root that QuartzCore's default.metallib lies in.

    Other frameworks' libraries are under the same root.
    """
    return str(qc)[: -len(QC_PATH) - 1] if str(qc).endswith(QC_PATH) else ''


# The Core Animation states the pack holds (ca/caset.py's tiers): every one. The runtime makes a state and places its
# programs in the shader heap at its first lookup (raw_gpu.cpp Device::pipeline), so the pack's size is file size only.
CA_TIERS = ('corpus', 'manifest', 'variants', 'blend', 'fetch')


def ca_corpus(qc: Path, corpus: Path, work: Path) -> Path:
    """Give the corpus with its Core Animation lines made from their names, when ca/ has this QuartzCore's tables."""
    digest = hashlib.sha256(qc.read_bytes()).hexdigest()
    for path in sorted((HERE / 'ca').glob('qc-*.json')):
        with open(path) as f:
            if json.load(f).get('metallib_sha256') == digest:
                break
    else:
        print(f"rawpack: no QuartzCore tables for default.metallib {digest[:16]}: the corpus's states as they are")
        return corpus
    sys.path.insert(0, str(HERE / 'ca'))
    import caset  # noqa: PLC0415 (ca/ is only on the path here)
    import caspec  # noqa: PLC0415

    tables = caspec.Tables(str(path))
    out = work / 'corpus.ca.log'
    with open(corpus) as src, open(out, 'w') as dst:
        dst.writelines(line for line in src if not caspec.corpus_fields(line)[0])
        for tier, names in caset.labels(tables, caset.corpus_names(str(corpus), tables), CA_TIERS).items():
            dst.writelines(caspec.state_line(tables, n) + '\n' for n in names)
            print(f'rawpack: Core Animation states, {tier}: {len(names)}')
    return out


def build(work: Path, bridge: Path, metal_abi: Path) -> None:
    """Build the mock GPU, the probe and the raw path's own states into `work`.

    The mock GPU with nvdec on every submission, the probe through the frontend + host bridge (beside it: rpath
    @executable_path) and the raw path's own states: the YCbCr conversions (ycbcr.metal) and the extended-range
    stand-ins (xr.metal), an iOS library like QuartzCore's, drawn after Core Animation's (probe.mm PROBE_OWN_LIB).
    """
    # Source paths (Mesa's asserts' __FILE__) as /switch-ios/..., whichever checkout or sandbox this runs in: the probe
    # and the mock are an action's outputs, and the run's cache key is their bytes.
    maps = [f'-ffile-prefix-map={RAW.parents[2]}=/switch-ios/tree', f'-ffile-prefix-map={Path.cwd()}=/switch-ios']
    cflags = ['-O2', '-Wall', *maps, '-I', ABI]
    objs = []
    for src in ('mesa/mme_fermi.c', 'mesa/mme_fermi_sim.c'):
        obj = work / (Path(src).stem + '.o')
        run(['xcrun', 'clang', '-O2', '-std=c11', '-w', *maps, '-I', HERE / 'mesa', '-c', HERE / src, '-o', obj])
        objs.append(obj)
    run(['xcrun', 'clang', *cflags, '-c', HERE / 'mock_iokit.c', '-o', work / 'mock_iokit.o'])
    run(['xcrun', 'clang++', *cflags, '-std=c++17', '-c', HERE / 'cap.cpp', '-o', work / 'cap.o'])
    run(['xcrun', 'clang++', *cflags, '-std=c++17', '-c', HERE / 'nvdec.cpp', '-o', work / 'nvdec.o'])
    run(
        [
            'xcrun',
            'clang++',
            '-dynamiclib',
            work / 'mock_iokit.o',
            work / 'cap.o',
            work / 'nvdec.o',
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
            '-O2',
            '-std=c++17',
            '-fobjc-arc',
            '-Wno-deprecated-declarations',
            *maps,
            '-I',
            METAL,
            '-I',
            metal_abi.parent,
            '-x',
            'objective-c++',
            HERE / 'probe.mm',
            METAL / 'NativeBridgeMetalFrontend.mm',
            '-x',
            'none',
            bridge,
            '-L',
            work,
            '-lmock_iokit',
            '-Wl,-rpath,@executable_path',
            '-lobjc',
            '-framework',
            'Foundation',
            '-framework',
            'Metal',
            '-framework',
            'IOSurface',
            '-framework',
            'IOKit',
            '-framework',
            'CoreVideo',
            '-o',
            work / 'probe',
        ]
    )
    own = ('ycbcr', 'xr')
    for name in own:
        run(
            ['xcrun', '-sdk', 'iphoneos', 'metal', '-c', HERE / f'{name}.metal', '-I', HERE, '-o', work / f'{name}.air']
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
    dirs = []
    for i, part in enumerate(lines):
        d = work / f'bucket{i}'
        d.mkdir(parents=True)
        (d / 'corpus.log').write_text(warm + ''.join(part))
        dirs.append(d)
    return dirs


def state_key(line: str) -> str:
    """Give an R line's state key as probe.mm prints it: every descriptor field but the label, then its functions.

    No lib= field is in it; a line whose first library is another framework's default.metallib than QuartzCore's ends
    with " | l <framework>" (probe.mm libraryPath, frameworkName).
    """
    libs = LIBRARY.findall(line)
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
    key = (' '.join(fields) + ' ' + line[bar1 : bar3 if bar3 >= 0 else len(line)]).strip()
    return f'{key} | l {framework}' if framework else key


def barrier_set(d: Path) -> Path | None:
    """Write <d>/barrier/corpus.log: the warm-up line and the lines of the states d's run drew with the interlock on."""
    sys.path.insert(0, str(HERE))
    import probelog  # noqa: PLC0415  the pack directory's module, on the path only now

    # the interlock on in any of a probe's draws: a fetched color whose texture lacks STORAGE is drawn in a scratch
    # between m2v's copy in and copy out (mkpack.py interlocked)
    keys = {
        p['key']
        for p in probelog.load(str(d / 'probe256.out'), str(d / 'probe256.log'))
        if any(r and r.get('regs', {}).get(0x1224, 0) & 3 for r in p['records'])
    }
    lines = (d / 'corpus.log').read_text().splitlines(keepends=True)
    chosen = [line for line in lines[1:] if state_key(line.rstrip('\n')) in keys]
    if not chosen:
        return None
    barrier = d / 'barrier'
    barrier.mkdir(exist_ok=True)
    (barrier / 'corpus.log').write_text(lines[0] + ''.join(chosen))
    return barrier


def probe_bucket(tools: Path, d: Path, nvk: Path, qc: Path, env: dict[str, str]) -> str:
    """Probe one bucket's states, then its storage-form states again with the interlock hidden (barrier_set).

    A run that only fills its m2v cache with NVK's driver records (NAK's programs), then the four decoded runs, which
    name each bound program by those records (RAWCAP_PROGRAMS) and hit the same cache.
    """
    said = [probe_runs(tools, d, nvk, qc, env)]
    barrier = barrier_set(d)
    if barrier:
        said.append(probe_runs(tools, barrier, nvk, qc, dict(env, NVK_DEBUG='no_fsi')))
    return '; '.join(said)


def probe_runs(tools: Path, d: Path, nvk: Path, qc: Path, env: dict[str, str]) -> str:
    """Run the warm-up and the four decoded probe runs over d's corpus."""
    if d.name == 'bucket0':
        env = dict(env, PROBE_OWN_LIB=str(tools / 'own.metallib'))
    said = []

    def probe(name: str, extra: dict[str, str]) -> None:
        with open(d / f'{name}.out', 'w') as out:
            r = subprocess.run(
                [str(tools / 'probe'), str(nvk), str(d / 'cache'), str(qc), str(d / 'corpus.log')],
                env=dict(env, PROBE_LIB_ROOT=library_root(qc), **extra),
                stdout=out,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
        if r.returncode:
            sys.exit(f'rawpack: {d.name} probe {name} failed:\n{r.stderr[-4000:]}')
        tail = [line for line in r.stderr.splitlines() if line.startswith('probe:')]
        said.append(f'{name}: {tail[-1] if tail else "?"}')

    shutil.rmtree(d / 'cache', ignore_errors=True)
    (d / 'cache').mkdir()
    probe('warm', {})
    run([sys.executable, HERE / 'mkprograms.py', d / 'programs.bin', d / 'cache'])
    for size in (256, 16, 4096):
        probe(
            f'probe{size}',
            {
                'PROBE_BYTES': str(size),
                'RAWCAP_LOG': str(d / f'probe{size}.log'),
                'RAWCAP_PROGRAMS': str(d / 'programs.bin'),
            },
        )
    probe(
        'probeoff',
        {
            'PROBE_BYTES': '256',
            'PROBE_BIND_OFFSET': '1',
            'RAWCAP_LOG': str(d / 'probeoff.log'),
            'RAWCAP_PROGRAMS': str(d / 'programs.bin'),
        },
    )
    shutil.rmtree(d / 'cache')
    return f'rawpack: {d.parent.name + "/" if d.name == "barrier" else ""}{d.name}: ' + '; '.join(said)


def probe_env() -> dict[str, str]:
    """Give the probe's environment: this process's, less every knob of the stack, plus the probe's own."""
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
    return env


def stamp(pack_bin: Path, stack: int) -> None:
    """Write the stack key into pack.bin's header."""
    pack = bytearray(pack_bin.read_bytes())
    struct.pack_into('<Q', pack, 8 + 16 + 8 * 2 * 8, stack)  # PackHeader.stack (after the section table)
    pack_bin.write_bytes(pack)


def link(out: Path, buckets: list[Path], stack: int, programs: Path) -> None:
    """mkpack.py over the buckets' probe runs: out/pack.bin, stamped with the stack key."""
    out.mkdir(parents=True, exist_ok=True)
    report = run([sys.executable, HERE / 'mkpack.py', programs, out, *buckets])
    if stack:
        stamp(out / 'pack.bin', stack)
    print(report.rstrip())


def stamp_or_link(stack: int) -> bool:
    """Run the --stamp or --link step where the command line names one; return whether it did."""
    if '--stamp' in sys.argv[1:]:  # --stamp <pack.bin> --stack <key>
        stamp(Path(option('--stamp')), stack)
        return True
    if '--link' in sys.argv[1:]:  # --link --out <dir> [--stack <key>] <bucket dir>...
        out = Path(option('--out')).resolve()
        buckets = [Path(a).resolve() for a in sys.argv[sys.argv.index('--link') + 1 :] if Path(a).is_dir()]
        buckets = [b for b in buckets if b != out]
        link(out, buckets, stack, out / 'programs.bin')
        return True
    return False


def main() -> None:
    if '--build-only' in sys.argv[1:]:
        out = Path(option('--out')).resolve()
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory() as scratch:
            build(Path(scratch), Path(option('--bridge')).resolve(), Path(option('--metal-abi')).resolve())
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
            probe_bucket(
                Path(option('--prebuilt')).resolve(), Path(option('--probe-bucket')).resolve(), nvk, qc, probe_env()
            )
        )
        return
    corpus = Path(option('--corpus')).resolve()
    out = Path(option('--out')).resolve()
    work = out / 'work'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    corpus = ca_corpus(qc, corpus, work)
    if '--split' in sys.argv[1:]:  # --split --qc <metallib> --corpus <corpus.log> --out <dir>: <dir>/work/bucket<i>
        split_buckets(corpus, work, BUCKETS)
        return
    prebuilt = option('--prebuilt', False)
    if prebuilt:
        for name in BUILT:
            shutil.copyfile(Path(prebuilt) / name, work / name)
            (work / name).chmod(0o755)
    else:
        build(work, Path(option('--bridge')).resolve(), Path(option('--metal-abi')).resolve())
    env = probe_env()
    buckets = split_buckets(corpus, work, BUCKETS)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
        for line in pool.map(lambda b: probe_bucket(work, b, nvk, qc, env), buckets):
            print(line)
    link(out, buckets, stack, work / 'programs.bin')
    if '--clean' in sys.argv[1:]:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
