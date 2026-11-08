#!/usr/bin/env python3
"""Prepare iOS for a Switch: out/sd, a folder holding one install image per iOS version.

    bazel run //:image [-- --out DIR]                   switchroot/ios/IOS17.SII.* (--config=ios27: IOS27.SII.*)
    bazel run //:image_dry_run                          the plan, without the IPSW's large members
    bazel run //:image_to_card -- diskN [--yes]         put the folder's images on the card (to_card.py)

No card is needed: the container is composed for 1 TiB and install-ios.bin fits it to the card's iOS partition, which
must be at least the image's minimum. Every stage is kept with the key of what it was made from (keys.py), so a rerun
does only what changed. Nothing needs root: file owners are given in the detached image (owners.py).
"""

import argparse
import json
import os
import resource
import shutil
import sys
import time
from collections.abc import Callable
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.common import paths
from tools.install import (
    apfs,
    card as cards,
    disks,
    extents,
    folder,
    gpt,
    host,
    keys,
    manifest,
    progress,
    space,
    stages,
)

INPUTS = (
    'spec',
    'ipsw',
    'os_dmg',
    'system_cryptex',
    'app_cryptex',
    'boot_package',
    'gpu_base',
    'gpu_stager',
    'shader_seed',
    'shader_seed_size',
    'install_payload',
    'pretranslated_pins',
    'apfs_tool',
    'delta_os',
    'delta_app_cryptex',
    'delta_system_cryptex',
    'translated_app_cryptex',
    'translated_system_cryptex',
)
BAZEL_OUTPUTS = (
    'delta_os',
    'delta_app_cryptex',
    'delta_system_cryptex',
    'translated_app_cryptex',
    'translated_system_cryptex',
)


def resolve_inputs(args: argparse.Namespace) -> None:
    for listing in args.inputs or []:
        for name, value in json.loads(Path(paths.located(listing)).read_text()).items():
            if not getattr(args, name, None):
                setattr(args, name, value)
    for name in INPUTS:
        setattr(args, name, paths.located(getattr(args, name, None)) or None)
    if args.gpu_base and os.path.isfile(args.gpu_base):
        args.gpu_base = os.path.dirname(args.gpu_base)


def plan_lines(args: argparse.Namespace, first: str | None, name: str) -> list[str]:
    if first == 'head':
        return [f"write {name}.SII.001's head (manifest and boot files); the rest is unchanged"]
    steps = [
        f'compose a 1 TiB iOS container in {args.scratch}/{stages.COMPOSED} (kept for the next run)',
        'populate Data and Preboot on a clone of it, owners given offline',
        f'prove the fit on a clone, pack {args.out}/{folder.SII_DIR}/{name}.SII.*',
    ]
    start = ('compose', 'populate', 'pack').index(first) if first else len(steps)
    stop = ('compose', 'populate').index(args.stop_after) + 1 if args.stop_after else len(steps)
    return [('unchanged: ' if i < start else '') + line for i, line in enumerate(steps[:stop])]


def check_inputs(args: argparse.Namespace) -> None:
    names = ['spec', 'ipsw', 'boot_package', 'gpu_base', 'gpu_stager', 'install_payload', 'apfs_tool']
    if not args.dry_run and not args.boot_only:
        names += ['os_dmg', 'system_cryptex', 'app_cryptex']
    missing = [n for n in names if not getattr(args, n) or not os.path.exists(getattr(args, n))]
    if missing:
        raise host.HostError('inputs missing: {}'.format(', '.join(missing)))


class Timer:
    """Each stage's wall time and the CPU it and its children used, for the log."""

    def __init__(self, log: host.Log) -> None:
        self.log = log

    @staticmethod
    def cpu() -> float:
        own, children = resource.getrusage(resource.RUSAGE_SELF), resource.getrusage(resource.RUSAGE_CHILDREN)
        return own.ru_utime + own.ru_stime + children.ru_utime + children.ru_stime

    def __call__(self, stage: str, work: Callable[[], object]) -> object:
        wall, cpu = time.monotonic(), self.cpu()
        with progress.step(stage, unit=None):
            result = work()
        wall, cpu = time.monotonic() - wall, self.cpu() - cpu
        cores = os.cpu_count() or 1
        share = 100 * cpu / max(wall, 1e-9) / cores
        self.log(f'time: {stage} {wall:.1f} s wall, {cpu:.1f} s CPU ({share:.0f}% of {cores} cores)')
        return result


class Builder:
    def __init__(self, args: argparse.Namespace, log: host.Log):
        self.args, self.log = args, log
        self.pins = json.loads(Path(args.ipsw).read_text())
        self.spec = stages.ipsw_volumes(json.loads(Path(args.spec).read_text()), self.pins)
        self.name, self.label = folder.image_name(self.pins), stages.version_label(self.pins)
        self.time = Timer(log)

    def run(self) -> int:
        check_inputs(self.args)
        files = stages.boot_files(self.args.boot_package)
        out = Path(self.args.out) if self.args.dry_run else folder.prepare(self.args.out)
        record = folder.records(out).get(self.name)
        if not (out / folder.SII_DIR / f'{self.name}.SII.001').is_file():
            record = None
        if self.args.boot_only:
            return self.boot_only(out, record, files)
        inputs = stages.key_inputs(self.args, self.spec, self.pins)
        k = self.time('keys', lambda: keys.compute(inputs, files, self.label))
        work = stages.Stages(self.args, self.spec, self.pins, self.log)
        first = keys.first_stage(k, work.state(), record)
        self.log(f'image: {self.label} as {self.name}.SII')
        for line in plan_lines(self.args, first, self.name):
            self.log('  plan:', line)
        if self.args.dry_run or first is None:
            self.log('dry run: nothing written' if self.args.dry_run else f'{self.name}.SII: unchanged')
            return 0
        work.clean()
        if first != 'head':
            cached = sum(space.allocated(getattr(self.args, n, None)) for n in BAZEL_OUTPUTS)
            space.check_run(self.args.scratch, work.space_needed(first), cached, self.log)
        if first == 'head':
            self.time('head', lambda: stages.refresh_head(out, self.name, files, self.label))
            self.finish(out, k)
            return 0
        if not self.compose_and_populate(work, k, first):
            return 0
        m = self.time('pack', lambda: work.pack(self.args.apfs_tool, out, self.name, files))
        self.finish(out, k, m.minimum_bytes)
        self.log(f'folder: {out}; {self.name} needs a partition of {m.minimum_bytes / (1 << 30):.1f} GiB or more')
        return 0

    def compose_and_populate(self, work: stages.Stages, k: keys.Keys, first: str) -> bool:
        """Run compose and populate as far as `first` and --stop-after ask; return whether pack comes next."""
        if first == 'compose':
            self.time('compose', lambda: work.compose(k.compose))
        if self.args.stop_after == 'compose':
            return False
        if first in ('compose', 'populate'):
            self.time('populate', lambda: work.populate(k.populate))
        return self.args.stop_after != 'populate'

    def boot_only(self, out: Path, record: dict | None, files: list[tuple[str, Path]]) -> int:
        if not record:
            raise host.HostError(f'{out} holds no {self.name}.SII //:image finished: run //:image first')
        current = stages.boot_record(self.args)
        changed = [k for k in current if current[k] != record.get('boot', {}).get(k)]
        if changed:
            raise host.HostError(
                "the boot package's {} changed since the image was made (it lives on Data): run //:image".format(
                    ', '.join(changed)
                )
            )
        stages.refresh_head(out, self.name, files, self.label)
        self.add_shared(out)
        folder.record(out, self.name, dict(record, head=None))
        self.log(f'folder: the head of {self.name}.SII.001 and install-ios.bin refreshed in {out}')
        return 0

    def add_shared(self, out: Path) -> None:
        entry = cards.boot_entry_files(self.args.boot_package)
        for relative in (cards.INI, cards.ICON):
            if relative in entry:
                folder.add(out, relative, entry[relative])
        folder.add(out, folder.PAYLOAD, self.args.install_payload)

    def finish(self, out: Path, k: keys.Keys, minimum: int | None = None) -> None:
        before = folder.records(out).get(self.name, {})
        entry = {'pack': k.pack, 'head': k.head, 'label': self.label, 'boot': stages.boot_record(self.args)}
        entry['minimum_bytes'] = minimum or before.get('minimum_bytes')
        folder.record(out, self.name, entry)
        kept = Path(self.args.scratch) / stages.KEPT
        if kept.is_dir():
            shutil.copytree(kept, Path(out) / 'pretranslated', dirs_exist_ok=True)
            shutil.rmtree(kept)
        self.add_shared(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', help='the folder to write (default: out/sd in the source tree)')
    parser.add_argument('--boot-only', action='store_true', help="rewrite only the image's boot files (the head)")
    parser.add_argument('--stop-after', choices=('compose', 'populate'), help='stop after this stage; rerun to go on')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--scratch', help='where the container is composed and kept (sparse; about 15 GB used)')
    parser.add_argument('--log')
    parser.add_argument('--inputs', action='append', help='a JSON file of the inputs below (//install:image_inputs)')
    for name in INPUTS:
        parser.add_argument('--' + name.replace('_', '-'))
    args = parser.parse_args(argv)
    resolve_inputs(args)
    args.out = os.path.abspath(args.out or paths.workspace() / 'out/sd')
    build = json.loads(Path(args.ipsw).read_text())['build']
    args.scratch = os.path.abspath(args.scratch or stages.default_scratch(build))
    args.log = args.log or str(paths.run_log('image'))
    log = host.Log(args.log) if not args.dry_run else PrintLog()
    try:
        host.check_tools(log)
        with disks.interrupts():
            return Builder(args, log).run()
    except cards.RefusedError as error:
        log('REFUSED:', error)
        return 2
    except (host.HostError, apfs.ApfsError, gpt.GptError, extents.ExtentError, manifest.ManifestError) as error:
        log('FAILED:', error)
        return 1
    finally:
        log.quiet('log:', args.log)


class PrintLog:
    def __call__(self, *parts: object) -> None:
        progress.println(' '.join(str(p) for p in parts))

    def quiet(self, *parts: object) -> None:
        pass


if __name__ == '__main__':
    sys.exit(main())
