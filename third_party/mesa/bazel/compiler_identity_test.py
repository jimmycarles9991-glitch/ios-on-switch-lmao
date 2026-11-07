"""compiler_identity.py: which edits make NVK's compiler identity new, over a stand-in Mesa tree."""

import tempfile
import unittest
from pathlib import Path

from third_party.mesa.bazel import compiler_identity

SAME = (
    'src/nouveau/vulkan/nvk_image.c',
    'src/nouveau/vulkan/nvk_image_view.c',
    'src/nouveau/vulkan/nvk_device_memory.c',
    'src/nouveau/vulkan/nvk_heap.c',
    'src/nouveau/vulkan/nvk_cmd_draw.c',
    'src/nouveau/vulkan/nvkmd/darwin/nvkmd_darwin.c',
    'src/nouveau/nil/nil_image.c',
    'README.rst',
)
NEW = (
    'src/compiler/nir/nir.c',
    'src/compiler/spirv/spirv_to_nir.c',
    'src/nouveau/compiler/nak/lib.rs',
    'src/nouveau/compiler/BUILD.bazel',
    'src/nouveau/vulkan/nvk_shader.c',
    'src/nouveau/vulkan/nvk_nir_lower_descriptors.c',
    'src/nouveau/vulkan/nvk_descriptor_set_layout.c',
    'src/nouveau/vulkan/nvk_cmd_buffer.h',
    'src/vulkan/runtime/vk_pipeline.c',
    'src/vulkan/runtime/vk_shader.c',
    'src/util/blob.c',
    'bazel/rust.bzl',
    'VERSION',
)


class CompilerIdentityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.mesa = Path(self.tmp.name) / 'third_party/mesa'
        for relative in SAME + NEW:
            (self.mesa / relative).parent.mkdir(parents=True, exist_ok=True)
            (self.mesa / relative).write_text(relative + '\n')

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def identity(self) -> str:
        return compiler_identity.identity(self.mesa, [self.mesa / r for r in SAME + NEW])

    def test_image_memory_and_other_edits_keep_it(self) -> None:
        before = self.identity()
        for relative in SAME:
            (self.mesa / relative).write_text('edited\n')
            self.assertEqual(self.identity(), before, relative)

    def test_compiler_edits_make_it_new(self) -> None:
        for relative in NEW:
            before = self.identity()
            (self.mesa / relative).write_text('edited\n')
            self.assertNotEqual(self.identity(), before, relative)


if __name__ == '__main__':
    unittest.main()
