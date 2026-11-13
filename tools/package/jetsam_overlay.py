"""The Supplemental cryptex's jetsam properties overlay, made of fragments.

iOS 27's launchd merges this one file into its jetsam database (its Bootstrap SupplementalJetsamProperties). Each
fragment is a Version4 plist of Daemon Override entries alone; the overlay holds every fragment's entries, and a label
two fragments name is refused.

usage: jetsam_overlay.py --out <plist> <fragment>...
"""

import argparse
import plistlib
from pathlib import Path


def merged(fragments: list[dict]) -> dict:
    """Give the overlay of the fragments' Daemon Override entries."""
    override = {}
    for fragment in fragments:
        daemon = fragment.get('Version4', {}).get('Daemon', {})
        if list(fragment) != ['Version4'] or list(fragment['Version4']) != ['Daemon'] or list(daemon) != ['Override']:
            raise ValueError('a fragment holds something besides Version4 Daemon Override entries')
        for label, entry in daemon['Override'].items():
            if label in override:
                raise ValueError(f'two fragments name {label}')
            override[label] = entry
    return {'Version4': {'Daemon': {'Override': override}}}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('fragments', type=Path, nargs='+')
    args = parser.parse_args()
    args.out.write_bytes(plistlib.dumps(merged([plistlib.loads(f.read_bytes()) for f in args.fragments])))


if __name__ == '__main__':
    main()
