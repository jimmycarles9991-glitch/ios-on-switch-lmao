"""python3 -m tools.benchloop.nakbatches [folder]: write the NAK question batches (batches/README.md) into the folder.

The files are what the generators give, so rerunning this after a generator changes is how a batch file changes.
"""

import json
import sys
from pathlib import Path

from tools.benchloop import protocol, store2fma
from tools.benchloop.sm53 import (
    affinity,
    dualclock,
    hsetp2,
    icache,
    int3,
    interlock_spin,
    overlap,
    powr_native,
    readers,
)

FOLDER = Path(__file__).with_name('batches')
# file: (batch name, generator), in the order a sitting runs them
BATCHES = {
    'sr_affinity': ('sr-affinity', affinity.generate),
    'int3_xmad': ('int3-xmad', int3.generate),
    'overlap': ('overlap', overlap.generate),
    'icache': ('icache', icache.generate),
    'store2fma': ('store2fma', store2fma.generate),
    'hsetp2': ('hsetp2', hsetp2.generate),
    'interlock_spin': ('interlock-spin', interlock_spin.generate),
    'icache_128k': ('icache-128k', icache.generate_large),
    'powr_native': ('powr-native', powr_native.generate),
    'nak_latency': ('nak-latency', readers.generate),
    'nak_writers': ('nak-writers', readers.generate_writers),
    'nak_yield': ('nak-yield', readers.generate_yield),
    'nak_dual': ('nak-dual', dualclock.generate),
}


def texts() -> dict[str, str]:
    """Give every batch file's text by its name, in the batch files' indented layout."""
    return {
        stem: json.dumps(protocol.batch(name, generate()), indent=2) + '\n'
        for stem, (name, generate) in BATCHES.items()
    }


def write(folder: Path) -> dict[str, int]:
    """Write every batch; give each file's size in bytes."""
    folder.mkdir(parents=True, exist_ok=True)
    sizes = {}
    for stem, text in texts().items():
        (folder / f'{stem}.json').write_text(text)
        sizes[stem] = len(text)
    return sizes


def main(argv: list[str]) -> int:
    folder = Path(argv[0]) if argv else FOLDER
    for stem, size in write(folder).items():
        print(f'{folder / stem}.json {size / 1e6:.2f} MB')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
