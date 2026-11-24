#!/usr/bin/env python3
"""Bazel action entry point that composes, populates and packs one install image (install/translated.bzl).

Runs in an ephemeral scratch directory beside --out (on the same APFS volume so cp -c clones work) and removes
scratch before exiting, so only the packed NAME.SII.* folder becomes the action output.
"""

import argparse
import os
import shutil
import sys
import tempfile
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.install import disks, host, image

TAIL_LINES = 40


class FileLog(host.Log):
    """Everything to the file only: Bazel shows an action's output, and a successful one has nothing to say."""

    def __call__(self, *parts: object) -> None:
        self.quiet(*parts)


def parse(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True)
    for name in image.INPUTS:
        parser.add_argument('--' + name.replace('_', '-'))
    return parser.parse_args(argv)


def prepare_args(args: argparse.Namespace, scratch: Path, log_path: Path) -> argparse.Namespace:
    for name in image.INPUTS:
        value = getattr(args, name, None)
        setattr(args, name, os.path.abspath(value) if value else None)
    if args.gpu_base and os.path.isfile(args.gpu_base):
        args.gpu_base = os.path.dirname(args.gpu_base)
    args.out = os.path.abspath(args.out)
    args.scratch = str(scratch)
    args.log = str(log_path)
    args.dry_run = False
    args.boot_only = False
    args.stop_after = None
    args.inputs = None
    return args


def run(args: argparse.Namespace) -> int:
    out = Path(os.path.abspath(args.out))
    out.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix='.image-scratch-', dir=out.parent))
    fd, log_name = tempfile.mkstemp(prefix='switch-image-pack-', suffix='.log')
    os.close(fd)
    log_path = Path(log_name)
    prepare_args(args, scratch, log_path)
    log = FileLog(log_path)
    try:
        host.check_tools(log)
        with disks.interrupts():
            return image.Builder(args, log).run()
    except host.HostError as error:
        tail = log_path.read_text(errors='replace').splitlines()[-TAIL_LINES:]
        sys.stderr.write('\n'.join(tail) + f'\nFAILED: {error}\n')
        return 1
    finally:
        try:
            disks.sweep(scratch, log)
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
            log_path.unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    return run(parse(argv))


if __name__ == '__main__':
    sys.exit(main())
