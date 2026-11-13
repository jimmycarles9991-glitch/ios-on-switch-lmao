#!/usr/bin/env python3
"""NVK's compiler identity: one hash over every Mesa source and flag that decides what a shader compiles to.

usage: compiler_identity.py --mesa <third_party/mesa> --header <nvk_build_id.h> --text <file> <source>...

NVK hashes it into its pipeline cache UUID in place of its own build ID (NVK_BUILD_ID_OVERRIDE, which the header defines
for nvk_physical_device.c alone), and m2v names its driver records by that UUID. So a seed bucket, or a device's
shader cache, made by one NVK is good for every NVK whose compiler is the same: an edit to NVK's image or memory code
keeps both, an edit to the compiler makes them all new.

Of the sources given (each by its path below --mesa), counted are: Mesa's version and its Bazel flags (the files'
text, the same for the iOS dylib and the macOS twin), NIR and the SPIR-V front end (src/compiler), NAK with its crate
flags (src/nouveau/compiler), the class headers its code and NVK's shader setup encode, util (blobs, hashes, the hash
tables NIR walks), the Vulkan runtime's pipelines, shaders, layouts and caches (src/vulkan/runtime, src/vulkan/util,
the registry), include/, and of NVK its headers, its shader, lowering, descriptor and sampler code, the physical
device (features and compiler flags) and the device and instance (drirc, app workarounds). Not counted: NVK's image,
buffer, memory, queue, command buffer and query code, its kernel driver backends, nil and the MME.
A file the rules miss compiles into NVK unhashed: a stale seed or cache hit, a correctness bug. When unsure, count it.
"""

import argparse
import hashlib
from pathlib import Path, PurePosixPath

TREES = (
    'include/',
    'src/compiler/',
    'src/nouveau/compiler/',
    'src/nouveau/headers/',
    'src/util/',
    'src/vulkan/registry/',
    'src/vulkan/runtime/',
    'src/vulkan/util/',
)
FILES = (
    'VERSION',
    'bazel/defs.bzl',
    'bazel/flags.bzl',
    'bazel/nouveau.bzl',
    'bazel/repos.bzl',
    'bazel/rust.bzl',
    'src/BUILD.bazel',
)
NVK = 'src/nouveau/vulkan/'
NVK_SOURCES = (
    '00-nvk-defaults.conf',
    'BUILD.bazel',
    'nvk_descriptor_set.c',
    'nvk_descriptor_set_layout.c',
    'nvk_descriptor_table.c',
    'nvk_device.c',
    'nvk_drirc_gen.py',
    'nvk_format.c',
    'nvk_instance.c',
    'nvk_nir_lower_descriptors.c',
    'nvk_nir_lower_mesh_shader.c',
    'nvk_physical_device.c',
    'nvk_sampler.c',
    'nvk_shader.c',
    'nvk_switch_push.c',
    'nvk_switch_compile.c',
    'nvk_switch_ir.c',
    'nvk_switch_ir_alu.c',
    'nvk_switch_ir_barrier.c',
    'nvk_switch_ir_decl.c',
    'nvk_switch_ir_glsl.c',
    'nvk_switch_ir_instr.c',
    'nvk_switch_ir_values.c',
    'nvk_switch_round.c',
    'nvk_switch_split.c',
    'nvk_switch_split_nir.c',
    'nvk_switch_split_prologue.c',
    'nvkmd/nvkmd.h',
)
NVK_TREES = ('layers/',)


def counted(relative: str) -> bool:
    """Whether the file at `relative` (below Mesa's root) is part of the compiler's identity."""
    if relative in FILES or relative.startswith(TREES):
        return True
    if not relative.startswith(NVK):
        return False
    own = relative[len(NVK) :]
    return own in NVK_SOURCES or own.startswith(NVK_TREES) or ('/' not in own and own.endswith('.h'))


def identity(mesa: Path, sources: list[Path]) -> str:
    """Hash every counted source, by its path below `mesa` and its bytes, in path order."""
    digest = hashlib.sha256(b'nvk compiler identity 1\n')
    relatives = sorted({PurePosixPath(source.relative_to(mesa)).as_posix() for source in sources})
    for relative in relatives:
        if counted(relative):
            content = hashlib.sha256((mesa / relative).read_bytes()).hexdigest()
            digest.update(f'{relative}\0{content}\n'.encode())
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter, fromfile_prefix_chars='@'
    )
    parser.add_argument('--mesa', type=Path, required=True)
    parser.add_argument('--header', type=Path, required=True)
    parser.add_argument('--text', type=Path, required=True)
    parser.add_argument('sources', type=Path, nargs='+')
    args = parser.parse_args()
    build_id = identity(args.mesa, args.sources)
    args.header.write_text(f'#define NVK_BUILD_ID_OVERRIDE "{build_id}"\n')
    args.text.write_text(build_id + '\n')


if __name__ == '__main__':
    main()
