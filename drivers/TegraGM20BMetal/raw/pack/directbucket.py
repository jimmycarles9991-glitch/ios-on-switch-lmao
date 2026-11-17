#!/usr/bin/env python3
"""directbucket: one bucket of the raw pack made directly (directpack.mm), as rawpack.py's buckets are made.

usage: directbucket.py --probe-bucket <bucket dir> --prebuilt <dir> --qc <default.metallib> [--root <dir>]
                       [--data-libs <dir>]
makes one bucket in place: records.bin (its states' and kernels' records), and bucket 0's context.log; a line's lib=
path is under --root (the crawl's root/ with the build's applications), else QuartzCore's, and a lib=data:<SHA-256>
library is --data-libs/<SHA-256>.metallib (else the crawl's data/ beside --root).

It imports none of the generator's modules: a bucket's run is a function of its states and the tools alone, and the
build keys each bucket on this file, so an edit to the pack's format, split or link reruns no bucket.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

QC_PATH = 'System/Library/Frameworks/QuartzCore.framework/default.metallib'
# Lines the bridge's direct compile writes to stderr (NativeMetalDirect.rs translate): the build's output carries them
DIRECT_COMPILE_SAYS = 'direct compile: '


def option(name: str, required: bool = True) -> str | None:
    if name in sys.argv[1:-1]:
        return sys.argv[sys.argv.index(name) + 1]
    if required:
        sys.exit(__doc__)
    return None


def library_root(qc: Path) -> str:
    """Give the crawl root that QuartzCore's default.metallib lies in.

    Other frameworks' libraries are under the same root.
    """
    return str(qc)[: -len(QC_PATH) - 1] if str(qc).endswith(QC_PATH) else ''


def bucket(tools: Path, d: Path, qc: Path, env: dict[str, str], selectors: str | None = None) -> str:
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
    tables = [selectors] if selectors else []
    step('directpack', [tools / 'directpack', qc, corpus, d / 'records.bin', *tables], own, d / 'directpack.out')
    if d.name == 'bucket0':
        shutil.copyfile(tools / 'context.log', d / 'context.log')
    shutil.rmtree(cache, ignore_errors=True)
    return '\n'.join([f'rawpack: {d.name}: ' + '; '.join(said), *bridge_said])


def probe_env() -> dict[str, str]:
    """Give the probe's environment: this process's, less every knob of the stack, plus the probe's own.

    PROBE_LIB_ROOT is --root and PROBE_DATA_LIBS --data-libs where the command line names them.
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
    if option('--data-libs', False):
        env['PROBE_DATA_LIBS'] = str(Path(option('--data-libs')).resolve())
    return env


def main() -> None:
    tools, bucket_dir = Path(option('--prebuilt')).resolve(), Path(option('--probe-bucket')).resolve()
    selectors = option('--ca-selectors', False)
    print(bucket(tools, bucket_dir, Path(option('--qc')).resolve(), probe_env(), selectors))


if __name__ == '__main__':
    main()
