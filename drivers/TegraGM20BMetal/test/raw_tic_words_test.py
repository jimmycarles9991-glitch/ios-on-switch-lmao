"""raw's colour target format and texture header word 0 of each format are NIL's.

raw/runtime/raw_zeta.h (with SET_ZT_FORMAT) and raw_gpu.cpp's kFormats, each row against NIL's row of its format
(nil_formats.csv through nil_format_table_gen.py; kits/SwitchGM20B/caps/formats.tsv names each Metal format's), as NVK
takes both words from it, and a depth aspect sampled through a view that keeps the image's format and a stencil aspect
through one of util_format_stencil_only's (nvk_image_view.c), raw_stencil_bits.h's kStencilViews. The words are rebuilt
here from that row and the class headers by the generator's rules.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
ZETA = ROOT / 'drivers/TegraGM20BMetal/raw/runtime/raw_zeta.h'
FORMATS = ROOT / 'drivers/TegraGM20BMetal/raw/runtime/raw_gpu.cpp'
STENCIL = ROOT / 'drivers/TegraGM20BMetal/raw/runtime/raw_stencil_bits.h'
NIL = ROOT / 'third_party/mesa/src/nouveau/nil/nil_formats.csv'
CLASSES = ROOT / 'third_party/mesa/src/nouveau/headers/nvidia/classes'

# metal2vulkan's Vulkan format of each Metal depth/stencil format, as a pipe format
PIPE = {250: 'Z16_UNORM', 252: 'Z32_FLOAT', 253: 'S8_UINT', 260: 'Z32_FLOAT_S8X24_UINT'}
# util_format_stencil_only of each with stencil
STENCIL_ONLY = {'S8_UINT': 'S8_UINT', 'Z32_FLOAT_S8X24_UINT': 'X32_S8X24_UINT'}
# The extended-range formats' codes as raw renders and fetches them (raw_xr.h; kFormats names them)
XR_PIPES = {550: 'R32G32_UINT', 552: 'R16G16B16A16_UINT', 554: 'R32_UINT'}


def defines(path: Path) -> dict[str, int]:
    out = {}
    for line in path.read_text().splitlines():
        m = re.match(r'#define\s+(\w+)\s+(0x[0-9a-fA-F]+)\s*$', line)
        if m:
            out[m.group(1)] = int(m.group(2), 16)
    return out


def nil_rows() -> dict[str, list[str]]:
    rows = {}
    for line in NIL.read_text().splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        fields = [f.strip() for f in line.split(',')]
        rows[fields[0]] = fields
    return rows


def tic0(row: list[str], tex: dict[str, int]) -> int:
    _, _, tcs, types, _, srcs = row[:6]
    word = tex['NVB197_TEXHEAD_BL_COMPONENTS_SIZES_' + tcs]  # (the B097 header's codes, ASTC's among them)
    names = {'S': 'SNORM', 'N': 'UNORM', 'I': 'SINT', 'U': 'UINT', 'F': 'FLOAT'}
    for c in range(4):
        kind = types[c] if c < len(types) else types[0]
        word |= tex['NV9097_TEXHEADV2_0_R_DATA_TYPE_NUM_' + names[kind]] << (7 + 3 * c)
    for c, s in enumerate(srcs):
        if s == '1':
            first = next(types['RGBA'.find(x)] for x in srcs if x in 'RGBA')
            value = tex['NV9097_TEXHEADV2_0_Z_SOURCE_IN_ONE_' + ('FLOAT' if first in 'SNF' else 'INT')]
        else:
            value = tex['NV9097_TEXHEADV2_0_Z_SOURCE_IN_' + {'0': 'ZERO'}.get(s, s)]
        word |= value << (19 + 3 * c)
    return word


class RawTicWordsTest(unittest.TestCase):
    def test_colour_words_and_targets_are_nils(self):
        tex = defines(CLASSES / 'clb097tex.h') | defines(CLASSES / 'cl9097tex.h')
        tex = {k.replace('NVB097_', 'NVB197_'): v for k, v in tex.items()}
        threed = defines(CLASSES / 'clb197.h')
        pipes = {}
        for line in (ROOT / 'kits/SwitchGM20B/caps/formats.tsv').read_text().splitlines():
            if line and not line.startswith('#'):
                metal, _, pipe, *_ = line.split('\t')
                pipes[int(metal)] = pipe
        pipes.update(XR_PIPES)
        rows = nil_rows()
        body = FORMATS.read_text().split('const FormatInfo kFormats[] = {', 1)[1].split('};', 1)[0]
        table = re.findall(
            r'\{\s*(\d+),\s*(0x[0-9a-f]+|0),\s*(0x[0-9a-f]+),\s*\d+,\s*(true|false)'
            r'(?:,\s*\d+,\s*\d+)?\s*\}',
            body,
        )
        self.assertEqual(len(table), body.count('{'))
        self.assertGreaterEqual(len(table), 90)
        for metal, target, word, srgb in table:
            row = rows[pipes[int(metal)]]
            with self.subTest(metal=metal, pipe=row[0]):
                want = 0 if row[1] == 'NONE' else threed['NVB197_SET_COLOR_TARGET_FORMAT_V_' + row[1]]
                self.assertEqual(int(target, 16), want)
                self.assertEqual(int(word, 16), tic0(row, tex))
                self.assertEqual(srgb == 'true', 'SRGB' in row[0])

    def test_zeta_words_are_nils(self):
        tex = defines(CLASSES / 'clb197tex.h') | defines(CLASSES / 'cl9097tex.h')
        threed = defines(CLASSES / 'clb197.h')
        rows = nil_rows()
        row_pattern = r'\{(\d+), (0x[0-9a-f]+), \d+, 0x[0-9a-f]+, (?:true|false), (?:true|false), (0x[0-9a-f]+)\}'
        table = re.findall(row_pattern, ZETA.read_text())
        self.assertTrue(table)
        for metal, zt, word in table:
            row = rows[PIPE[int(metal)]]
            with self.subTest(metal=metal, pipe=row[0]):
                self.assertEqual(int(zt, 16), threed['NVB197_SET_ZT_FORMAT_V_' + row[1]])
                self.assertEqual(int(word, 16), tic0(row, tex))

    def test_stencil_views_are_nvks(self):
        tex = defines(CLASSES / 'clb197tex.h') | defines(CLASSES / 'cl9097tex.h')
        rows = nil_rows()
        table = re.findall(r'\{(\d+), (0x[0-9a-f]+)\},\s*// \w+: (\w+)', STENCIL.read_text())
        self.assertEqual(sorted(int(m) for m, _, _ in table), [253, 260])
        for metal, word, pipe in table:
            with self.subTest(metal=metal, pipe=pipe):
                self.assertEqual(pipe, STENCIL_ONLY[PIPE[int(metal)]])
                self.assertEqual(int(word, 16), tic0(rows[pipe], tex))

    def test_block_compressed_words_are_nils(self):
        tex = defines(CLASSES / 'clb097tex.h') | defines(CLASSES / 'cl9097tex.h')
        tex = {k.replace('NVB097_', 'NVB197_'): v for k, v in tex.items()}
        rows = nil_rows()
        row_pattern = r'\{(\d+), 0, (0x[0-9a-f]+), \d+, (true|false), \d+, \d+\},\s*// \w+: (\w+)'
        table = re.findall(row_pattern, FORMATS.read_text())
        self.assertGreaterEqual(len(table), 40)
        for metal, word, srgb, pipe in table:
            with self.subTest(metal=metal, pipe=pipe):
                self.assertEqual(int(word, 16), tic0(rows[pipe], tex))
                self.assertEqual(srgb == 'true', 'SRGB' in pipe)


if __name__ == '__main__':
    unittest.main()
