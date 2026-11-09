#!/usr/bin/env python3
"""The trust cache bundle the kernel window carries: one static module.

    trust.py --static MODULE [--module FILE]... --out BUNDLE

--static is the IPSW's StaticTrustCache payload, whose UUID the module keeps; each --module is merged into it: the
cryptexes' trust caches (checked against the BuildManifest by //ipsw), the overlay's own code (overlay.py trust; an
empty file adds nothing), the GPU bundle's and iOS 27's pretranslated trees'.

The loader stands in for iBoot, which vouches for the StaticTrustCache it loads, and XNU loads the bundle's first
module as kTCTypeStatic in every configuration. On iOS libignition loads the cryptexes' trust caches as Cryptex1 types
against a manifest personalized to the device, which this boot cannot have; any further module would load as an
engineering trust cache, which RELEASE kernels skip.
"""

import argparse
import struct
from pathlib import Path

from tools.trustcache.trustcache import merged_module, module_entries


def trust_cache_bundle(module: bytes) -> bytes:
    """Return the /chosen/memory-map/TrustCache bundle of one module: u32 count 1, u32 offset 8, the module."""
    module_entries(module)
    return struct.pack('<II', 1, 8) + module


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('static', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--module', action='append', default=[], type=Path)
    args = parser.parse_args()
    modules = [args.static.read_bytes()] + [m for m in (p.read_bytes() for p in args.module) if m]
    args.out.write_bytes(trust_cache_bundle(merged_module(modules)))


if __name__ == '__main__':
    main()
