import contextlib
import io
import json
import re
import struct
import tempfile
import unittest
from pathlib import Path

import libraries

STAGED = '/private/var/staged_system_apps/Example.app/Extensions/Scene.appex/default.metallib'
INSTALLED = (
    '/private/var/containers/Bundle/Application/01234567-89AB-CDEF-0123-456789ABCDEF/'
    'Example.app/Extensions/Scene.appex/default.metallib'
)


class ResolutionTest(unittest.TestCase):
    def test_installed_system_app_uses_the_selected_os_staged_library(self):
        self.assertEqual(libraries.library_path(INSTALLED, {STAGED}), STAGED)
        self.assertEqual(libraries.library_path(INSTALLED.removeprefix('/private'), {STAGED}), STAGED)

    def test_missing_or_distinct_application_is_not_guessed(self):
        self.assertEqual(libraries.library_path(INSTALLED, set()), INSTALLED)
        other = INSTALLED.replace('Example.app', 'Other.app')
        self.assertEqual(libraries.library_path(other, {STAGED}), other)
        digest = 'data:' + '1' * 64
        self.assertEqual(libraries.library_path(digest, {STAGED}), digest)

    def test_exact_installed_library_takes_precedence(self):
        self.assertEqual(libraries.library_path(INSTALLED, {STAGED, INSTALLED}), INSTALLED)

    def test_cryptex_and_escaped_paths_resolve_without_changing_constants(self):
        path = '/System/Library/Frameworks/Example Kit.framework/default.metallib'
        line = 'R ok=1 | v entry lib=/System/Cryptexes/OS' + path.replace(' ', '%20') + ' #7:33:02000000 | '
        self.assertEqual(libraries.relocate(line, {path}), line.replace('/System/Cryptexes/OS', ''))

    def test_dispatch_and_linked_stages_resolve_together(self):
        table = {'kernel': [STAGED], 'helper': [STAGED]}
        lines = [
            'D ok=1 tg=32,1,1 edge=0,0,0 threads=0 | form=d opt=0 | k kernel | l helper | ',
            f'R ok=1 | v kernel lib={INSTALLED} | f - | fl helper lib={INSTALLED} as=selected | ',
        ]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'names.json').write_text(json.dumps(table))
            (root / 'input.log').write_text('\n'.join(lines) + '\n')
            with contextlib.redirect_stdout(io.StringIO()):
                libraries.resolve(str(root / 'names.json'), str(root / 'input.log'), str(root / 'output.log'))
            result = (root / 'output.log').read_text().splitlines()
        self.assertEqual(
            result[0],
            lines[0].replace('k kernel', 'k kernel lib=' + STAGED).replace('l helper', 'l helper lib=' + STAGED),
        )
        self.assertEqual(result[1], lines[1].replace(INSTALLED, STAGED))


def _library(functions: list[str]) -> bytes:
    entries = b''
    for name in functions:
        tags = b'NAME' + struct.pack('<H', len(name) + 1) + name.encode() + b'\0' + b'ENDT'
        entries += struct.pack('<I', 4 + len(tags)) + tags
    header = bytearray(b'MTLB' + bytes(0x54))
    struct.pack_into('<QQ', header, 24, 0x58, len(entries))
    return bytes(header) + struct.pack('<I', len(functions)) + entries


def _universal(slices: list[tuple[int, int, bytes]]) -> bytes:
    at = 8 + 20 * len(slices)
    head, body = b'\xca\xfe\xba\xbe' + struct.pack('>I', len(slices)), b''
    for cpu, subtype, data in slices:
        head += struct.pack('>5I', cpu, subtype, at + len(body), len(data), 0)
        body += data
    return head + body


class AirSliceTest(unittest.TestCase):
    # 24B119's CoreImage.metallib: an AIR 2.9 slice (subtype 0xd) beside an AIR 2.4 one (8)
    FAT = _universal(
        [
            (0x01000013, 0, b'\xcf\xfa\xed\xfe'),
            (libraries.AIR64, 0xD, _library(['new', 'old'])),
            (libraries.AIR64, 8, _library(['old'])),
        ]
    )

    def test_each_build_names_the_slice_its_metal_loads(self):
        self.assertEqual(libraries.names(self.FAT, '21H461'), ['new', 'old'])
        self.assertEqual(libraries.names(self.FAT, '24B119'), ['old'])
        self.assertEqual(libraries.names(self.FAT), ['new', 'old'])

    def test_a_profile_7_device_takes_air_2_9_only_without_an_older_slice(self):
        only = _universal([(libraries.AIR64, 0xD, _library(['new']))])
        self.assertEqual(libraries.names(only, '24B119'), ['new'])
        self.assertIsNone(libraries.names(_universal([(libraries.AIR64, 0x20010, _library(['x']))]), '24B119'))

    def test_the_rules_are_metallib_slices(self):
        header = Path('drivers/NativeMetal/MetallibSlice.h').read_text()
        rows = re.findall(r'\{"(\w+)", (true|false), (0x[0-9a-f]+|\d+), (true|false), (0x[0-9a-f]+|\d+)\}', header)
        self.assertEqual(
            {build: (first == 'true', int(c, 0), low == 'true', int(a, 0)) for build, first, c, low, a in rows},
            libraries.AIR_SLICE_RULES,
        )


if __name__ == '__main__':
    unittest.main()
