"""rules_rust's bootstrap compile remaps the directory it ran in out of the rlib it writes, under NVK's Rust too.

rust-src in that toolchain's sysroot puts ~1,000 -L directories on the command line, so Bazel moves rustc's arguments
into a params file, and the bootstrap wrapper must expand --remap-path-prefix=${pwd}=. there as well (rules_rust.patch).
Unexpanded, tinyjson's rlib named the sandbox it was compiled in, and so did process_wrapper's UUID: every Rust compile
of NVK got another action key on each machine and in each output base.
"""

import os
import unittest
from pathlib import Path

RLIBS = [Path(path) for path in os.environ['RLIBS'].split()]


def around(data: bytes, needle: bytes) -> bytes:
    at = data.find(needle)
    return data[max(0, at - 80) : at + 80] if at >= 0 else b''


class BootstrapRemap(unittest.TestCase):
    def test_tinyjson_names_its_sources_from_the_execution_root(self):
        [rlib] = [path for path in RLIBS if path.name.startswith('libtinyjson-')]
        data = rlib.read_bytes()
        self.assertEqual(around(data, b'/execroot/'), b'')
        self.assertTrue(
            b'./external/rules_rust++i+rules_rust_tinyjson/src/parser.rs' in data, 'no remapped source path'
        )


if __name__ == '__main__':
    unittest.main()
