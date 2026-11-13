#!/usr/bin/env python3
"""Write the raw GM20B path's stack key.

usage: raw_stack.py @<params>   with --ipsw <build> --out <file> and --file <path below the tree>=<path>, one per file

The stack key is known before the plugin is compiled: it is compiled in (-DTEGRA_RAW_STACK), stamped into the raw pack
and the native seed, and names each process's record cache, so a new key retires every record made before. It hashes the
IPSW build and each file gpu/defs.bzl raw_stack names (what records are made and read by), each by its path below the
tree, its length and its bytes, in path order. Writes the first 16 hex digits.
"""

import argparse
import hashlib
from pathlib import Path

SCHEME = b'raw stack 2'


def key(ipsw: str, files: dict[str, Path]) -> str:
    """Give the stack key of `files` (each file's path below the tree: where it is) under IPSW build `ipsw`."""
    digest = hashlib.sha256(SCHEME + b'\0' + ipsw.encode() + b'\0')
    for name in sorted(files):
        data = files[name].read_bytes()
        digest.update(name.encode() + b'\0' + len(data).to_bytes(8, 'little') + data)
    return digest.hexdigest()[:16]


def main() -> None:
    parser = argparse.ArgumentParser(fromfile_prefix_chars='@')
    parser.add_argument('--ipsw', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--file', action='append', default=[], help='<path below the tree>=<path>')
    args = parser.parse_args()
    files = dict(entry.split('=', 1) for entry in args.file)
    if len(files) != len(args.file):
        raise SystemExit('raw_stack.py: a path below the tree is named twice')
    Path(args.out).write_text(key(args.ipsw, {name: Path(path) for name, path in files.items()}) + '\n')


if __name__ == '__main__':
    main()
