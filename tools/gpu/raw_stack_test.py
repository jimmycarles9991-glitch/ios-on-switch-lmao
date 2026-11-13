"""The raw stack key hashes what the raw path's records are made and read by, and none of the rest of its runtime.

Over the paths //gpu:raw_stack hashes (its output group `inputs`) and the files below raw/.
"""

import ast
import re
import shutil
import tempfile
import unittest
from pathlib import Path

from tools.gpu import raw_stack

HASHED = Path('gpu/raw_stack.inputs')
RAW = Path('drivers/TegraGM20BMetal/raw')
PACK = RAW / 'pack'
RUNTIME = RAW / 'runtime'
FILE_NAME = re.compile(r'[\w./+-]{1,200}')

UNKEYED = [
    RUNTIME / 'RawMetalSchedule.mm',
    RUNTIME / 'raw_submit_queue.h',
    RUNTIME / 'raw_gpu.cpp',
    RUNTIME / 'raw_stats.h',
    RUNTIME / 'raw_compiler.h',
    RUNTIME / 'raw_compiler.cpp',
    RUNTIME / 'RawComplete.mm',
]

KEYED = [
    RUNTIME / 'raw_cache.cpp',
    RUNTIME / 'raw_cache_io.h',
    RUNTIME / 'raw_pack.h',
    RUNTIME / 'raw_record.h',
    RUNTIME / 'raw_direct.cpp',
    RUNTIME / 'RawDirect.mm',
    RUNTIME / 'RawRenderKey.mm',
    RUNTIME / 'raw_completed.cpp',
    PACK / 'rawpack.py',
    PACK / 'packfile.py',
    PACK / 'native_seed.mm',
    Path('drivers/NativeMetal/NativeBridgeMetalFrontend.mm'),
    Path('drivers/NativeMetal/abi/MetalABI.h'),
    Path('drivers/NativeMetal/bridge/NativeMetalDirect.rs'),
    Path('third_party/m2v/crates/metal2vulkan/src/lib.rs'),
    Path('third_party/m2v/crates/metal2vulkan-runtime/src/lib.rs'),
]


def hashed() -> set[str]:
    return set(HASHED.read_text().split())


def named_files(source: Path) -> list[Path]:
    """Give the files below raw/ that a generator module names by a string or imports as a module of raw/pack."""
    names = []
    for node in ast.walk(ast.parse(source.read_text())):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            text = node.value
            if FILE_NAME.fullmatch(text) and not text.startswith('/') and '..' not in Path(text).parts:
                names += [PACK / text, RAW / text, RUNTIME / text, RUNTIME / f'{text}.cpp', RUNTIME / f'{text}.mm']
                names.append(PACK / f'{text}.metal')
        elif isinstance(node, ast.Import):
            names += [PACK / f'{alias.name}.py' for alias in node.names]
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            names.append(PACK / f'{node.module}.py')
    return [path for path in names if path.is_file()]


class Inputs(unittest.TestCase):
    def test_runtime_work_and_the_packs_choice_of_states_are_not_hashed(self):
        keyed = hashed()
        choice = [path for path in (PACK / 'ca').rglob('*') if path.is_file()]
        self.assertTrue(choice)
        for path in UNKEYED + choice:
            self.assertTrue(path.is_file(), path)
            self.assertNotIn(str(path), keyed)

    def test_the_record_makers_and_readers_are_hashed(self):
        keyed = hashed()
        for path in KEYED:
            self.assertIn(str(path), keyed)
        self.assertTrue(any(path.endswith('/libvulkan_nouveau.dylib') for path in keyed))
        self.assertTrue(any('rust_crates__ash-' in path for path in keyed), "the bridge's crates.io crates")

    def test_the_pack_generator_builds_and_reads_only_hashed_files(self):
        keyed = hashed()
        modules = [Path(path) for path in keyed if path.startswith(f'{PACK}/') and path.endswith('.py')]
        self.assertIn(PACK / 'rawpack.py', modules)
        for module in modules:
            for path in named_files(module):
                with self.subTest(module=module.name):
                    self.assertIn(str(path), keyed)


class Key(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        for path in [*hashed(), *map(str, UNKEYED)]:
            if not path.startswith('../') and Path(path).is_file():
                (self.root / path).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, self.root / path)

    def key(self, ipsw: str = 'one build') -> str:
        """Give the copy's key over the paths the rule hashes that this test has (not NVK or the crates.io crates)."""
        return raw_stack.key(ipsw, {name: self.root / name for name in hashed() if (self.root / name).is_file()})

    def edited(self, path: Path) -> str:
        """Give the copy's key with `path` edited, then put `path` back."""
        target = self.root / path
        original = target.read_bytes()
        target.write_bytes(original + b'\n// edited\n')
        try:
            return self.key()
        finally:
            target.write_bytes(original)

    def test_a_runtime_edit_keeps_the_key(self):
        before = self.key()
        for path in UNKEYED:
            with self.subTest(path=path.name):
                self.assertEqual(self.edited(path), before)

    def test_a_record_maker_edit_changes_the_key(self):
        before = self.key()
        for path in KEYED:
            if (self.root / path).is_file():
                with self.subTest(path=path.name):
                    self.assertNotEqual(self.edited(path), before)

    def test_each_ios_has_its_own_key(self):
        self.assertNotEqual(self.key('one build'), self.key('another build'))


if __name__ == '__main__':
    unittest.main()
