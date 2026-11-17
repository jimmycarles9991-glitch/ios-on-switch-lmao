"""The stage compile's object: its only global definitions are the six entry points.

Every name it leaves undefined is one libSystem or libc++ exports, so no Mesa, NAK or Rust name of it can bind to
another copy in the image that links it (NAK's Rust carries its own standard library).

usage: switch_compile_object_test.py <nm> (<object> <SDK>)...
"""

import re
import subprocess
import sys
import unittest
from pathlib import Path

ENTRY_POINTS = {
    '_switch_compile_abi',
    '_switch_compile_ranked_stage',
    '_switch_compile_split_parts',
    '_switch_compile_stage',
    '_switch_compile_stage_finish',
    '_switch_compile_stage_metrics',
}
SYSTEM_LIBRARIES = ('libSystem.B.tbd', 'libc++.1.tbd', 'libc++abi.tbd')
NAME = re.compile(r"'?(_[\w$.]+)'?")


def names(nm: str, flags: list[str], obj: Path) -> set[str]:
    out = subprocess.run([nm, *flags, obj], capture_output=True, text=True, check=True).stdout
    return {line.split()[-1] for line in out.splitlines() if line.strip()}


def system_names(sdk: Path) -> set[str]:
    found: set[str] = set()
    for library in SYSTEM_LIBRARIES:
        found |= set(NAME.findall((sdk / 'usr/lib' / library).read_text()))
    return found


class SwitchCompileObjectTest(unittest.TestCase):
    def test_each_object(self) -> None:
        nm = sys.argv[1]
        pairs = list(zip(sys.argv[2::2], sys.argv[3::2], strict=True))
        self.assertTrue(pairs)
        for obj, sdk in pairs:
            with self.subTest(obj=obj):
                self.assertEqual(names(nm, ['-gU', '-j'], Path(obj)), ENTRY_POINTS)
                undefined = names(nm, ['-u', '-j'], Path(obj))
                self.assertEqual(sorted(undefined - system_names(Path(sdk))), [])


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1])
