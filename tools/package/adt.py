"""adt.py SEED_HEX OUT: the native OLED board's ADT for the full-RAM root-boot profile.

native_oled_adt.generate_adt makes it. Its only nondeterminism, the /chosen/random-seed, is pinned (//boot:adt), so the
result is the same on every build; the loader mixes a per-boot seed into it (boot/loader/seed.h).
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import native_oled_adt

seed, out = sys.argv[1:]
Path(out).write_bytes(
    native_oled_adt.generate_adt(
        bytes.fromhex(seed), native_oled_adt.Profile(storage_probe=True, root_boot=True, full_ram=True)
    )
)
