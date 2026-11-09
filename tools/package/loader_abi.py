"""What the native loader (boot/loader) and the packaging agree on.

Addresses, the RAM ledger's exclusions, the ADT properties the loader fills, and the staged kernel window's framing
(boot/loader/stage.h).
"""

# The kernel window: the trust caches from WINDOW_BASE up, directly below the kernel as XNU wants them
# (native_xnu_layout.kernel_phys_base), then the kernel from KERNEL_PHYS_BASE (kernel/switch/include/switch/
# boot_layout.h), where its lowest segment, the prelinked AMFI closure at VA ...0427c000, keeps its 2 MiB offset.
# 770 MiB take the window to 0xE2200000, the block-rounded envelope the loader checks against the live FDT.
WINDOW_BASE = 0xB2000000
KERNEL_PHYS_BASE = 0xB207C000
WINDOW_SIZE = 770 << 20

# Kept out of XNU's RAM by the loader (the full-RAM profile's ledger).
NATIVE_EXCLUSIONS = [
    ('original-fdt', 0x80000000, 0x20000),
    ('working-fdt', 0x8D000000, 0x40000),
    ('boot-args', 0x8FE00000, 0x20000),
    ('adapter-stack-vectors', 0xA0000000, 0x20000),
    ('staged-kernel-capacity', 0xA2000000, 0x6000000),
    # hekate loads all of bl33.bin here: U-Boot in the first MiB, then the staged kernel window (boot/loader/stage.h).
    ('bl33-and-kernel-window', 0xAA000000, 0x6000000),
    ('ramoops', 0xB0000000, 0x200000),
    ('retained-firmware-display-carveouts', 0xF0000000, 0x10000000),
]

NATIVE_FULL_RAM_EXCLUSIONS = [r for r in NATIVE_EXCLUSIONS if r[0] != 'retained-firmware-display-carveouts'] + [
    ('framebuffer', 0xF5A00000, 0x400000),
    ('secure-low-tail', 0xFFD00000, 0x300000),
]

# boot_args Video (base, display, row bytes, width, height, depth): hekate's framebuffer in graphics mode, where XNU
# never draws, so hekate's boot logo stays on the panel until backboardd draws the same one. Display 0 is the text
# console.
VIDEO = [0xF5A00000, 1, 2880, 720, 1280, 0x320]

# The cacheable-copy loader ships the window packed: only its nonzero extents (4 KiB granular; zero gaps shorter than
# 64 KiB stay in an extent), back to back. Over half the window is zeros (the VA holes between prelinked segments); the
# loader writes them itself during its cached copy. Each extent is also LZ4-compressed (lz4 -12, independent blocks of
# 1 MiB, ~2.4:1 on this kernel); the loader decodes it under the same cached regime (boot/loader/copy.c lz4_block).
# 1 MiB, not 4: the slowest block takes lz4 -12 0.5 s, not 2.5 s, for 0.2% more bytes (lz4's window is 64 KiB).
PACK_PAGE = 0x1000
PACK_MIN_GAP = 0x10000
LZ4_BLOCK = 1 << 20
# The packaging compresses the blocks in shards, one action each: a block's shard is the MiB of the window it starts in,
# so a block keeps its shard (and an unchanged shard its cached compression) whatever changes elsewhere in the window.
WINDOW_SHARD_BYTES = 1 << 20
# The most the loader decodes into the kernel window (boot/loader/copy.h LOADER_WINDOW_BYTES_MAX).
WINDOW_BYTES_MAX = 128 << 20
WINDOW_SHARDS = WINDOW_BYTES_MAX // WINDOW_SHARD_BYTES
LZ4_MAGIC = b'\x04\x22\x4d\x18'

# /chosen/switch-boot-stamps: boot/loader/stamps.h SWITCH_STAMP_COUNT 64-bit words, zero until the loader fills it.
STAMPS_PROPERTY = 'switch-boot-stamps'
STAMPS_BYTES = 26 * 8
STAMPS_SLOT = 0xA001EF00
# /chosen/random-seed (the pinned bytes) and /chosen/switch-seed-source (8 zero bytes): the loader mixes SE2's DRBG
# output into the first and reports in the second (boot/loader/seed.h SWITCH_SEED_BYTES: what xnu-12377 reads).
SEED_PROPERTY = 'random-seed'
SEED_BYTES = 256
SEED_SOURCE_PROPERTY = 'switch-seed-source'
# iBoot's /chosen/boot-manifest-hash: the SHA-384 of what authorised this boot. Here the boot is the kernel and the
# trust cache bundle the window carries (no Apple manifest), hashed by the layout step into this zeroed property.
# iOS 27's Image4 entangles its nonce magazine with it and refuses to start without one.
BOOT_MANIFEST_HASH_PROPERTY = 'boot-manifest-hash'
BOOT_MANIFEST_HASH_BYTES = 48
SEED_SOURCE_BYTES = 8

# The fixed same-boot SD reference page (boot/storage/sd_reference.h): the loader's code, data and stacks end below.
SD_REFERENCE = 0xA001F000
SD_REFERENCE_BYTES = 0x1000

# The window rides in bl33.bin after U-Boot (boot/loader/stage.h): hekate reads it with U-Boot at SDR104.
BL33_BASE = 0xAA000000
STAGE_HEADER = 0xAA100000
STAGE_SOURCE = 0xAA101000
STAGE_END = 0xB0000000
STAGE_WORDS = {'magic': 0x48435753, 'head': 0x4E49574B, 'tail': 0x444E454B, 'version': 1}


# What else rides in bl33.bin's first MiB, past U-Boot's image and pre-relocation BSS: (address, capacity). The boot
# script is sourced and the loader and exporter booted where they are; the DTB is copied out to 0x8D000000.
SCRIPT_SLOT = (0xAA070000, 0x10000)
LOADER_SLOT = (0xAA080000, 0x30000)
EXPORTER_SLOT = (0xAA0B0000, 0x10000)
DTB_SLOT = (0xAA0C0000, 0x40000)
# Where bootm loados copies the exporter and the boot script runs it (exporter.uimg's load and entry,
# boot/storage/firmware_export/link.ld), and its 64 KiB cap, its stack included. It sits in 'staged-kernel-capacity',
# which nothing else uses: the kernel window is staged in bl33.bin.
EXPORTER_RUN = (0xA7000000, 0x10000)

# switchroot/ios/bootargs.txt's page (boot/loader/boot_args.h).
BOOT_ARGS_PAGE = 0x8FE00000
BOOT_ARGS_BYTES = 0x1000


def staged_bl33_bytes(staged_bytes: int) -> int:
    """Return bl33.bin's size with a staged window of staged_bytes.

    U-Boot's MiB, the header page, the window, the trailer page (build_native_loader.py stage_into_bl33).
    """
    return STAGE_SOURCE - BL33_BASE + staged_bytes + PACK_PAGE
