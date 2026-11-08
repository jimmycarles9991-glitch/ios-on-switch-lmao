#!/usr/bin/env python3
"""The kext prelink of a fileset kernelcache (iPadOS 26/27) into our 12377 kernel, as one build step: the kexts of
kcinfo.ROOTS' closure, translated in kernel mode and linked against our kernel by name, in the main tree's prelink
format (emit26.py). Runs classify.py, pcrel.py, names.py, emit26.py and verify26.py over a work directory holding
the ipsw CLI's views of the kernelcache.

usage: prelink.py --kernel K --kernelcache KC --kernelcache-17 KC17 --arm64e CLI --ipsw IPSW --symbolsets PLIST
                  --signatures VERSION/xnu.json --slide SLIDE --out OUT --report REPORT --names NAMES

SLIDE is the slide the drivers' bindings (kits/SwitchABI's tables) assume: a prelink that moves the kexts elsewhere
fails here instead of linking addresses that miss.
"""

import argparse
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).absolute().parent


def run(env, *argv, stdout=None):
    subprocess.run([str(a) for a in argv], env=env, check=True, stdout=stdout)


def symbol_set_names(plist):
    """Every name and prefix our kernel's symbol sets export (XNU's symbolsets.plist): the KPI names a kext may import."""
    sets = plistlib.loads(plist.read_bytes())['SymbolsSets']
    names = {(e.get('SymbolName') or e.get('SymbolPrefix') or '').strip() for s in sets for e in s.get('Symbols', [])}
    return sorted(n for n in names if n)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('kernel', 'kernelcache', 'kernelcache-17', 'arm64e', 'ipsw', 'symbolsets', 'signatures', 'out', 'report',
                 'names'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--slide', required=True, type=lambda v: int(v, 0))
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix='prelink-'))
    env = dict(
        os.environ,
        HOME=str(work),  # ipsw reads its configuration from $HOME: none, here
        PRELINK_WORK=str(work),
        PRELINK_KERNELCACHE=str(args.kernelcache.resolve()),
        PRELINK_OURS=str(args.kernel.resolve()),
        PRELINK_KERNELCACHE_17=str(args.kernelcache_17.resolve()),
        PRELINK_ARM64E=str(args.arm64e.resolve()),
        PRELINK_SLIDE=hex(args.slide),
        PYTHONPATH=os.pathsep.join([str(HERE)] + sys.path),
    )
    try:
        nm = subprocess.run(['xcrun', 'nm', str(args.kernel)], env=env, check=True, capture_output=True, text=True)
        (work / 'ours.nm').write_text(''.join(line + '\n' for line in nm.stdout.splitlines() if len(line.split()) == 3))
        with open(work / 'cpp26.json', 'wb') as cpp:
            run(env, args.ipsw, 'kernel', 'cpp', '--json', '--methods', args.kernelcache, stdout=cpp)
        signatures = args.signatures.resolve().parent.parent
        run(env, args.ipsw, 'kernel', 'symbolicate', '-q', '--json', '--signatures', signatures, '-o', work / 'symbolicate',
            args.kernelcache)
        (symbols,) = (work / 'symbolicate').glob('*.symbols.json')
        (work / 'sym').mkdir()
        symbols.rename(work / 'sym' / 'kernelcache.decompressed.symbols.json')
        (work / 'symbolsets26.txt').write_text(''.join(n + '\n' for n in symbol_set_names(args.symbolsets)))
        python = sys.executable
        run(env, python, HERE / 'classify.py', args.kernelcache, stdout=subprocess.DEVNULL)
        run(env, python, HERE / 'pcrel.py', args.kernelcache, stdout=subprocess.DEVNULL)
        run(env, python, HERE / 'names.py', stdout=subprocess.DEVNULL)
        out = work / 'kernel.macho'
        run(env, python, HERE / 'emit26.py', args.kernel, args.kernelcache, out)
        run(env, python, HERE / 'verify26.py', out, args.kernelcache, stdout=subprocess.DEVNULL)
        slide = json.loads((work / 'kernel.macho.json').read_text())['slide']
        if slide != args.slide:
            raise SystemExit('the kexts moved by {:#x}, the bindings assume {:#x}'.format(slide, args.slide))
        shutil.copyfile(out, args.out)
        shutil.copyfile(work / 'kernel.macho.json', args.report)
        shutil.copyfile(work / 'kernel-names.json', args.names)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
