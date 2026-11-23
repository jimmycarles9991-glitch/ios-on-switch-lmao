"""The stack key hashes what the driver's records are made and read by, and none of the rest of its runtime.

Over the paths //gpu:stack_key hashes (its output group `inputs`) and the files below raw/.
"""

import ast
import re
import shutil
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path

from tools.gpu import stack_key

HASHED = Path('gpu/stack_key.inputs')
METAL = Path('drivers/TegraGM20BMetal')
PACK = METAL / 'pack'
RUNTIME = METAL
FILE_NAME = re.compile(r'[\w./+-]{1,200}')
# The driver root's files that were never the runtime's
DRIVER_ROOT_ONLY = {
    METAL / n
    for n in ('BUILD.bazel', 'TegraGM20BMetalPlugin.mm', 'stage-tegra-gm20b-metal.py', 'check-dyld-cache-imports.py')
}

UNKEYED = [
    RUNTIME / 'TegraGM20BMetalSchedule.mm',
    RUNTIME / 'submit_queue.h',
    RUNTIME / 'gpu.cpp',
    RUNTIME / 'stats.h',
    RUNTIME / 'compiler.h',
    RUNTIME / 'compiler.cpp',
    RUNTIME / 'TegraGM20BMetalComplete.mm',
]

KEYED = [
    RUNTIME / 'gm20b_cache.cpp',
    RUNTIME / 'cache_io.h',
    RUNTIME / 'pack.h',
    RUNTIME / 'record.h',
    RUNTIME / 'direct.cpp',
    RUNTIME / 'TegraGM20BMetalDirect.mm',
    RUNTIME / 'TegraGM20BMetalRenderKey.mm',
    RUNTIME / 'completed.cpp',
    PACK / 'make_pack.py',
    PACK / 'packfile.py',
    PACK / 'native_seed.mm',
    Path('drivers/TegraGM20BMetal/precompile/ca-selectors/ca_selector_layout.h'),
    Path('drivers/TegraGM20BMetal/abi/MetalABI.h'),
    Path('drivers/TegraGM20BMetal/translate/NativeMetalDirect.rs'),
    Path('drivers/TegraGM20BMetal/translate/NativeMetalTranslate.rs'),
    Path('third_party/m2v/crates/metal2vulkan/src/lib.rs'),
]


# A token edit and a comment, by the language a file is read as
DECLARATION = {
    '.h': b'\nint edited;\n',
    '.cpp': b'\nint edited;\n',
    '.mm': b'\nint edited;\n',
    '.rs': b'\nconst EDITED: u8 = 0;\n',
    '.py': b'\nedited = 1\n',
}
COMMENT = {
    '.h': b'// edited\n',
    '.cpp': b'/* edited */\n',
    '.mm': b'// edited\n',
    '.rs': b'/* /* edited */ */\n',
    '.py': b'# edited\n',
}


def hashed() -> set[str]:
    return set(HASHED.read_text().split())


def runtime_units(tree: ast.AST) -> set[str]:
    """Give the strings of each tuple or list whose strings all name a runtime unit (DIRECT_RUNTIME's stems).

    A stem names a unit only there: since the units lost their raw_ prefix, words like 'draw' also name one.
    """
    units = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Tuple, ast.List)):
            stems = [e.value for e in node.elts if isinstance(e, ast.Constant) and isinstance(e.value, str)]
            if stems and all(any((METAL / f'{s}{ext}').is_file() for ext in ('.cpp', '.mm')) for s in stems):
                units.update(stems)
    return units


def named_files(source: Path) -> list[Path]:
    """Give the files below raw/ that a generator module names by a string or imports as a module of raw/pack."""
    tree = ast.parse(source.read_text())
    units = runtime_units(tree)
    names = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            text = node.value
            if FILE_NAME.fullmatch(text) and not text.startswith('/') and '..' not in Path(text).parts:
                names += [
                    PACK / text,
                    METAL / text,
                    *(METAL / f'{text}{ext}' for ext in ('.cpp', '.mm') if text in units),
                ]
                names.append(PACK / f'{text}.metal')
        elif isinstance(node, ast.Import):
            names += [PACK / f'{alias.name}.py' for alias in node.names]
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            names.append(PACK / f'{node.module}.py')
    return [path for path in names if path.is_file() and path not in DRIVER_ROOT_ONLY]


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
        self.assertTrue(any(path.endswith('/switch_compile.o') for path in keyed), 'the stage compile')
        self.assertTrue(any('rust_crates__ash-' in path for path in keyed), "the translation's crates.io crates")

    def test_the_pack_generator_builds_and_reads_only_hashed_files(self):
        keyed = hashed()
        modules = [Path(path) for path in keyed if path.startswith(f'{PACK}/') and path.endswith('.py')]
        self.assertIn(PACK / 'make_pack.py', modules)
        for module in modules:
            for path in named_files(module):
                with self.subTest(module=module.name):
                    self.assertIn(str(path), keyed)


class Canonical(unittest.TestCase):
    def same(self, name: str, a: str, b: str) -> bool:
        return stack_key.canonical(name, a.encode()) == stack_key.canonical(name, b.encode())

    def test_c_comments_and_runs_of_layout_go_and_every_gap_stays(self):
        self.assertTrue(
            self.same(
                'a.h',
                'int  f(int x,\n      int y) {\n    return x + y; // sum\n}',
                'int f(int x, int y) { return x + y; }',
            )
        )
        self.assertTrue(self.same('a.mm', 'x = 1; /* one\ntwo */', 'x = 1;'))
        self.assertFalse(self.same('a.cpp', 'a - -b;', 'a--b;'))
        self.assertFalse(self.same('a.cpp', 'f<g<int> > x;', 'f<g<int>> x;'))
        self.assertFalse(self.same('a.h', 'int x;', 'int y;'))

    def test_the_preprocessors_layout_keys_apart(self):
        self.assertFalse(self.same('a.h', '#define A (x)\n', '#define A(x)\n'))
        self.assertFalse(
            self.same('a.h', '#define S(x) #x\nconst char *s = S(a+b);', '#define S(x) #x\nconst char *s = S(a + b);')
        )
        self.assertFalse(self.same('a.h', '#include <a/b.h>\n', '#include <a / b.h>\n'))
        self.assertFalse(self.same('a.h', 'double d = 1e+2;', 'double d = 1e +2;'))
        self.assertFalse(self.same('a.h', 'int x = 1 .x;', 'int x = 1.x;'))

    def test_a_c_directive_ends_at_its_line_and_a_literal_is_kept_whole(self):
        self.assertFalse(self.same('a.h', '#define A 1\nint x;', '#define A 1 int x;'))
        self.assertTrue(self.same('a.h', '#define F(x) \\\n    (x)\nint y;', '#define F(x) (x)\nint y;'))
        self.assertFalse(self.same('a.h', 's = "a // b";', 's = "a";'))
        self.assertFalse(self.same('a.cpp', 'r = R"(a /* b */)";', 'r = R"(a)";'))
        self.assertTrue(self.same('a.h', "#error don't\nint x; // y", "#error don't\nint x;"))

    def test_rust_comments_nest_and_raw_strings_and_lifetimes_are_tokens(self):
        self.assertTrue(self.same('a.rs', '/* a /* b */ c */ fn f() {}', 'fn f(){}'))
        self.assertFalse(self.same('a.rs', 'let s = r#"a // "b" c"#;', 'let s = r#"a"#;'))
        self.assertTrue(
            self.same('a.rs', "fn f<'a>(s: &'a str) -> char { '/' } // c", "fn f<'a>(s: &'a str) -> char {'/'}")
        )

    def test_a_rust_doc_comment_is_an_attribute_and_stays(self):
        self.assertFalse(self.same('a.rs', '/// one\nfn f() {}', '/// two\nfn f() {}'))
        self.assertFalse(self.same('a.rs', '/** one */ fn f() {}', 'fn f() {}'))
        self.assertTrue(self.same('a.rs', '//// note\n/**/ fn f() {}', 'fn f() {}'))

    def test_python_is_its_syntax_tree(self):
        self.assertTrue(self.same('a.py', "x = ['a',  'b']  # c\n", 'x = [\n    "a",\n    "b",\n]\n'))
        self.assertFalse(self.same('a.py', 'x = 1\n', 'x = 2\n'))

    def test_other_files_are_their_bytes(self):
        self.assertFalse(self.same('a.json', '{"a": 1}', '{"a":1}'))


class Key(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        for path in [*hashed(), *map(str, UNKEYED)]:
            if not path.startswith('../') and Path(path).is_file():
                (self.root / path).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, self.root / path)

    def key(self, ipsw: str = 'one build') -> str:
        """Give the copy's key over the hashed paths this test has (not the stage compile or crates.io's)."""
        return stack_key.key(ipsw, {name: self.root / name for name in hashed() if (self.root / name).is_file()})

    def edited(self, path: Path, edit: Callable[[bytes], bytes] | None = None) -> str:
        """Give the copy's key with `path` edited (by default a declaration added), then put `path` back."""
        target = self.root / path
        original = target.read_bytes()
        target.write_bytes(edit(original) if edit else original + DECLARATION.get(path.suffix, b'\nedited\n'))
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

    def test_a_comment_or_a_reformat_of_a_record_maker_keeps_the_key(self):
        before = self.key()
        for path in KEYED:
            if path.suffix in COMMENT and (self.root / path).is_file():
                comment = COMMENT[path.suffix]
                with self.subTest(path=path.name):
                    self.assertEqual(self.edited(path, lambda data, c=comment: b'\n\n' + data + c), before)

    def test_each_ios_has_its_own_key(self):
        self.assertNotEqual(self.key('one build'), self.key('another build'))


if __name__ == '__main__':
    unittest.main()
