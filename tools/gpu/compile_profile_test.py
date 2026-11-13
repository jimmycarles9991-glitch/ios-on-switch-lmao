"""The build's compile profile held to the m2v runtime device of its shader tools.

The pack generator's offline compile makes its translator devices of the profile
(drivers/NativeMetal/bridge/compile_profile/<build>.json), which this runtime made them of before.
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REFREEZE = (
    'freeze it again: raw/compile-profile <libvulkan_nouveau.dylib> <cache dir> '
    '> drivers/NativeMetal/bridge/compile_profile/<build>.json'
)


class CompileProfile(unittest.TestCase):
    def test_the_profile_is_the_runtimes(self):
        tools = Path(os.environ['COMPILE_PROFILE_TOOLS'])
        with tempfile.TemporaryDirectory() as scratch:
            # dyld names an input by another of its links; the runtime opens a copy of its own
            twin = Path(scratch) / 'libvulkan_nouveau.dylib'
            shutil.copyfile(tools / 'libvulkan_nouveau.dylib', twin)
            cache = Path(scratch) / 'cache'
            cache.mkdir()
            r = subprocess.run(
                [tools / 'raw' / 'compile-profile', twin, cache, '--check'],
                capture_output=True,
                text=True,
                check=False,
                env={'PATH': '/usr/bin:/bin', 'HOME': scratch, 'MESA_SHADER_CACHE_DISABLE': 'true'},
            )
        self.assertEqual(r.returncode, 0, f'{r.stderr.strip()}\n{REFREEZE}')


if __name__ == '__main__':
    unittest.main()
