"""pack/kernels.py: a record viewing a texture slot in texels of another size is left out of the pack.

The runtime would fail its command buffer (runtime/view.h SlotView::None).
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kernels

KEY = 'k | tg=8,8,1 | tgmem= | tex=0:70:2:1:1:02030405,1:115:2:1:1:02030405 | smp='


def tex(slot: int, window: int) -> dict:
    return {'off': 16 * slot, 'kind': kernels.TEXTURE, 'slot': slot, 'window': window}


class ViewsOtherTexelSize(unittest.TestCase):
    def test_same_size_views_stay(self) -> None:
        # RGBA8 viewed as A8B8G8R8 (0x08), RGBA16Float as R16_G16_B16_A16 (0x03)
        self.assertEqual(kernels.views_other_texel_size(KEY, [tex(0, 0x08), tex(1, 0x03)]), [])

    def test_another_size_is_named(self) -> None:
        # RGBA8 (4 bytes) viewed as R16_G16_B16_A16 (8 bytes)
        self.assertEqual(kernels.views_other_texel_size(KEY, [tex(0, 0x03), tex(1, 0x03)]), [0])

    def test_no_window_or_unknown_format_is_not_judged(self) -> None:
        self.assertEqual(kernels.views_other_texel_size(KEY, [tex(0, 0), tex(5, 0x1D)]), [])

    def test_two_plane_slots_view_their_planes(self) -> None:
        # RGB10A8_2P_XR10 in two planes at slot 2: its words as R32 (0x0F), its alpha bytes as R8 (0x1D)
        key = KEY.replace(' | smp=', ',2:550:2:1:1:02030405:xr2p | smp=')
        plane = {'off': 48, 'kind': kernels.TEXTURE_PLANE, 'slot': 2, 'window': 0x1D}
        self.assertEqual(kernels.views_other_texel_size(key, [tex(2, 0x0F), plane]), [])
        self.assertEqual(kernels.views_other_texel_size(key, [tex(2, 0x04), plane]), [2])
        self.assertEqual(kernels.views_other_texel_size(key, [tex(2, 0x0F), {**plane, 'window': 0x0F}]), [2])

    def test_a_plane_at_a_texture_of_one_image_is_named(self) -> None:
        plane = {'off': 16, 'kind': kernels.TEXTURE_PLANE, 'slot': 1, 'window': 0x1D}
        self.assertEqual(kernels.views_other_texel_size(KEY, [tex(1, 0x03), plane]), [1])


if __name__ == '__main__':
    unittest.main()
