import os
import unittest
from pathlib import Path

from tools.gpu import bundle

RAW = ['_metal_rates', '_OBJC_CLASS_$_SwitchMetalTexture', '_OBJC_METACLASS_$_SwitchMetalDevice', '_switch_refusal_log']
TRANSLATOR = [
    '__ZN12metal2vulkan5lower5stage17h0123456789abcdefE',
    '_RNvNtCs5Qb1yU3gTz2_22native_metal_translate7offline7compile',
]
FALLBACK = [
    '_native_bridge_metal_open',
    '_OBJC_CLASS_$_NBTexture',
    '_OBJC_METACLASS_$_NBDeviceMethods',
    '_nmb_device_new',
    '_nmb_render_pipeline_direct',
    '__ZN20metal2vulkan_runtime6device6Device3new17h0123456789abcdefE',
    '_RNvNtCs4fqI2P2rA04_19native_metal_bridge7objects3new',
]


class M2vSymbols(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.translation = bundle.translation_entries(Path(os.environ['TRANSLATE_HEADER']))

    def test_the_translations_entries_are_its_declarations_not_its_comments(self) -> None:
        self.assertIn('nmb_offline_library_new', self.translation)
        self.assertIn('nmb_last_error', self.translation)
        self.assertNotIn('nmb_library_new', self.translation)
        self.assertNotIn('nmb_render_pipeline_direct', self.translation)

    def test_the_raw_path_and_the_translation_pass(self) -> None:
        kept = RAW + TRANSLATOR + ['_' + name for name in sorted(self.translation)]
        self.assertEqual(bundle.m2v_symbols(kept, self.translation), [])

    def test_the_runtime_the_bridge_and_the_frontend_are_held(self) -> None:
        self.assertEqual(bundle.m2v_symbols(RAW + FALLBACK, self.translation), sorted(FALLBACK))


if __name__ == '__main__':
    unittest.main()
