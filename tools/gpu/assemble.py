#!/usr/bin/env python3
"""Assemble //gpu:base, the directory the GPU stager (stage-tegra-gm20b-metal.py --build) and the image builder read.

usage: assemble.py --bundle D --trustcache F --trust-json F --out D

  ngpu/addons/TegraGM20BMetal.bundle/            the signed bundle
  TegraGM20BMetal.trustcache, trust.json
  gpu.stage                                       the stage manifest of ngpu/ (tools/gpu/stage_format.py)
"""

import argparse
import shutil
from pathlib import Path

from tools.common import hashing
from tools.gpu import stage_format


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ('bundle', 'trustcache', 'trust-json', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    out = Path(args.out)
    if out.exists():
        shutil.rmtree(out)
    addons = out / 'ngpu/addons'
    shutil.copytree(args.bundle, addons / 'TegraGM20BMetal.bundle', copy_function=shutil.copyfile)
    shutil.copyfile(args.trustcache, out / 'TegraGM20BMetal.trustcache')
    shutil.copyfile(args.trust_json, out / 'trust.json')
    # Regular files only: the inputs are followed, never linked.
    executable = addons / 'TegraGM20BMetal.bundle' / 'TegraGM20BMetal'
    for path in [out, *out.rglob('*')]:
        path.chmod(0o755 if path.is_dir() or path == executable else 0o644)
    files = sorted(p for p in (out / 'ngpu').rglob('*') if p.is_file())
    lines = [stage_format.tree_header()]
    for path, digest in zip(files, hashing.sha256_files(files), strict=True):
        name = path.relative_to(out).as_posix()
        lines.append(stage_format.file_record(name, digest, path.stat().st_size, path.stat().st_mode & 0o777, name))
    lines += stage_format.pref_records()
    (out / stage_format.TREE_MANIFEST).write_text(''.join(line + '\n' for line in lines))
    (out / stage_format.TREE_MANIFEST).chmod(0o644)


if __name__ == '__main__':
    main()
