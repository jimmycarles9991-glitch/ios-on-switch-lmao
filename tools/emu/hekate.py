"""What hekate leaves behind when it starts the iOS entry (l4t=1: bootloader/l4t/l4t.c launch_l4t, hekate 6.x).

There is no hekate in the emulator. Core 0 starts at EL3 in firmware/regwrite.S, which replays REGISTERS below and
jumps to bl31 as hekate's ccplex_boot_cpu0 does; the rest is memory hekate fills (the BL31 parameters, U-Boot's
environment and serial console) and the boot files it loads. The values are hekate's own: display/di.inl for the
display, l4t.c for the hand-off, and the memory controller as boot/loader/loader.c native_gate read it on hardware.
"""

import struct
from pathlib import Path

PMC = 0x7000E400
CAR = 0x60006000
MC = 0x70019000
DC = 0x54200000
FUSE_PAGE = 0x7000F000

BL31 = 0xFFF00000  # TZDRAM_BASE: hekate's 1 MiB TZDRAM at the top of the first 4 GiB
TZDRAM_BYTES = 1 << 20
TZ_PARAMS = 0xFFFFF000
BL33 = 0xAA000000
BL33_ENV = 0xA9FC0000
BL33_ENV_MAGIC = 0x33334C42
FRAMEBUFFER = 0xF5A00000
EXTRA_FEATURES = 0x52545845  # "EXTR"
IRAM_PARAMS = 0x4D415249  # "IRAM": BL31 reads its parameters from PMC secure scratch 108/109
# SECURE_SCRATCH108 before the l4t launch on a T210B01 ("T214"), which hekate moves to 112 and U-Boot requires.
SETUP_DONE = 0x34313254
PANEL_SAM_AMS699VC01 = 0x2050
UART_A = '70006000'

_PARAMS_V1 = 0x100
_PLAT_PARAMS = 0x200
_BL33_EP = 0x300


def dc(index: int, value: int) -> tuple[int, int]:
    return DC + index * 4, value


def car(offset: int, value: int) -> tuple[int, int]:
    return CAR + offset, value


def _bits(*bits: int) -> int:
    return sum(1 << b for b in bits)


GENERAL_UPDATE, WIN_ALL_UPDATE = 1 << 8, _bits(8, 9, 10, 11, 12)
GENERAL_ACT_REQ, WIN_ALL_ACT_REQ = 1 << 0, _bits(0, 1, 2, 3, 4)

# hw_init.c: the microsecond timer's divider for clk_m at 19.2 MHz.
TIMERS = [(0x60005014, 0x45F)]

# di.c display_init: the display's clocks out of reset (DISP1, host1x, DSI, MIPI_CAL) and PLLD at hekate's rate.
CLOCKS = [
    car(0x30C, _bits(24, 16)),  # RST_DEV_H_CLR: MIPI_CAL, DSI
    car(0x328, _bits(24, 16)),  # CLK_ENB_H_SET
    car(0x304, _bits(27, 28)),  # RST_DEV_L_CLR: DISP1, HOST1X
    car(0x320, _bits(27, 28)),  # CLK_ENB_L_SET
    car(0x284, 1 << 17),  # CLK_ENB_X_SET: UART_FST_MIPI_CAL
    car(0x448, 1 << 19),  # CLK_ENB_W_SET: DSIA_LP
    car(0x138, 2 << 29),  # CLK_SOURCE_DISP1: PLLD_OUT0
    car(0x0D0, (1 << 30) | (1 << 27) | (3 << 20) | (20 << 11) | 1),  # PLLD_BASE: enabled, locked, P 3 N 20 M 1
]

# di.inl _di_dc_init_config.
DC_INIT = [
    dc(0x40, 0),
    dc(0x43, _bits(2, 4, 6, 10)),
    dc(0x41, GENERAL_UPDATE),
    dc(0x41, GENERAL_ACT_REQ),
    dc(0x480, 0),
    dc(0x403, 0),
    dc(0x404, 0),
    dc(0x36, _bits(0, 2, 4, 6, 8, 16, 18)),
    dc(0x01, 1 << 8),
    dc(0x28, (1 << 8) | 9),
    dc(0x41, WIN_ALL_UPDATE),
    dc(0x41, WIN_ALL_ACT_REQ),
    dc(0x42, _bits(4, 5, 6, 7)),
    dc(0x700, 0),
    dc(0x70E, 0),
    dc(0x611, 0xF0),
    dc(0x612, 0x12A),
    dc(0x613, 0),
    dc(0x614, 0x198),
    dc(0x615, 0x39B),
    dc(0x616, 0x32F),
    dc(0x617, 0x204),
    dc(0x618, 0),
    dc(0x430, 8),
    dc(0x42F, 0),
    dc(0x307, 1 << 24),
    dc(0x309, 0),
    dc(0x4E4, 0),
    dc(0x300, 0),
    dc(0x41, WIN_ALL_UPDATE),
    dc(0x41, WIN_ALL_ACT_REQ),
    dc(0x716, (1 << 24) | 255),
    dc(0x31, 0),
    dc(0x402, 0),
    dc(0x32, 0),
    dc(0x41, WIN_ALL_UPDATE),
    dc(0x41, WIN_ALL_ACT_REQ),
]

# di.inl _di_dc_video_mode_config: the 720x1280 panel's timings and continuous display.
DC_VIDEO_MODE = [
    dc(0x405, 0),
    dc(0x406, (1 << 16) | 0),
    dc(0x407, (1 << 16) | 72),
    dc(0x408, (9 << 16) | 72),
    dc(0x40A, (10 << 16) | 136),
    dc(0x409, (1280 << 16) | 720),
    dc(0x431, _bits(0, 16)),
    dc(0x303, 0),
    dc(0x432, _bits(0, 2)),
    dc(0x32, 1 << 5),
    dc(0x41, GENERAL_UPDATE),
    dc(0x41, GENERAL_ACT_REQ),
    dc(0x41, GENERAL_UPDATE),
    dc(0x41, GENERAL_ACT_REQ),
    dc(0x00, (3 << 8) | 1),
    dc(0x00, (3 << 8) | 1),
    dc(0x41, GENERAL_UPDATE),
    dc(0x41, GENERAL_ACT_REQ),
]

# di.inl _di_winA_pitch: window A scanning hekate's 720x1280 B8G8R8A8 framebuffer.
WIN_A_PITCH = [
    dc(0x42, 1 << 4),
    dc(0x700, 0),
    dc(0x402, 1 << 29),
    dc(0x703, 0xC),
    dc(0x704, 0),
    dc(0x707, 0),
    dc(0x708, 0),
    dc(0x706, (1280 << 16) | (720 * 4)),
    dc(0x709, (0x1000 << 16) | 0x1000),
    dc(0x705, (1280 << 16) | 720),
    dc(0x70A, ((720 * 2) << 16) | (720 * 4)),
    dc(0x702, 0),
    dc(0x80B, 0),
    dc(0x800, FRAMEBUFFER),
    dc(0x806, 0),
    dc(0x808, 0),
    dc(0x700, 1 << 30),
    dc(0x32, 1 << 5),
    dc(0x41, GENERAL_UPDATE | (1 << 9)),
    dc(0x41, GENERAL_ACT_REQ | (1 << 1)),
]

# The memory controller as hekate (l4t.c _l4t_mc_config_carveout, 4 GiB in EMEM_CFG) and bl31 leave it, read on
# hardware by the loader's native gate: VPR and the TZ carveout register off, the SECFW, GPUFW and GPU WPR carveouts.
MEMORY_CONTROLLER = [
    (MC + offset, value)
    for offset, value in (
        (0x050, 0x1000),
        (0x670, 0),
        (0x674, 0),
        (0x9D4, 0),
        (0x9A0, 0xFFF00000),
        (0x9A4, 0),
        (0x9A8, 0),
        (0x648, 0),
        (0x64C, 0),
        (0x978, 0),
        (0xC0C, 0xFFE00000),
        (0xC10, 0),
        (0xC14, 8),
        (0xC5C, 0xFFD00000),
        (0xC60, 0),
        (0xC64, 2),
        (0xCAC, 0xFFD40000),
        (0xCB0, 0),
        (0xCB4, 6),
        (0xCFC, 0),
        (0xD00, 0),
        (0xD04, 0),
        (0xD4C, 0),
        (0xD50, 0),
        (0xD54, 0),
    )
]

# l4t.c launch_l4t: BL31's parameters through PMC secure scratch, the setup marker U-Boot checks, the panel's ID.
HANDOFF = [
    (PMC + 0xB08, TZ_PARAMS + _PARAMS_V1),
    (PMC + 0xB0C, TZ_PARAMS + _PLAT_PARAMS),
    (PMC + 0xB10, IRAM_PARAMS),
    (PMC + 0xB18, SETUP_DONE),
    (PMC + 0xB1C, PANEL_SAM_AMS699VC01),
]

REGISTERS = TIMERS + CLOCKS + DC_INIT + DC_VIDEO_MODE + WIN_A_PITCH + MEMORY_CONTROLLER + HANDOFF


def register_table(entry: int) -> bytes:
    """Return REGISTERS as regwrite.S reads them, then the address it jumps to."""
    table = b''.join(struct.pack('<II', address, value) for address, value in REGISTERS)
    return table + struct.pack('<IIQ', 0, 0, entry)


def tz_params(uart: int) -> bytes:
    """Return the BL31 parameter page at TZ_PARAMS: bl31_v1_params, plat_params_from_bl2 and BL33's entry point."""
    page = bytearray(0x1000)
    struct.pack_into('<BBHI5Q', page, _PARAMS_V1, 3, 1, 48, 0, 0, 0, 0, TZ_PARAMS + _BL33_EP, 0)
    flags = 0b11  # PMC non-secure, no SC7 base restriction
    struct.pack_into(
        '<QQiiQQQiIQQQQQ',
        page,
        _PLAT_PARAMS,
        TZDRAM_BYTES,
        BL31,
        uart,
        0,
        0,
        0,
        0,
        0,
        EXTRA_FEATURES,
        0,
        0,
        0,
        0,
        flags,
    )
    spsr_el2t = 1 << 3
    struct.pack_into('<BBHIQII8Q', page, _BL33_EP, 1, 1, 88, 1, BL33, spsr_el2t, 0, *([0] * 8))
    return bytes(page)


def bl33_with_serial(bl33: bytes) -> bytes:
    """Apply hekate's uart_port=1: UART-A enabled in U-Boot's DTB and its stdout/stderr (l4t.c BL33_DTB_SET_*)."""
    image = bytearray(bl33)
    dtb = struct.unpack_from('<I', image, 0x10)[0]
    if image[dtb : dtb + 4] != b'\xd0\x0d\xfe\xed':
        raise SystemExit('bl33.bin: no DTB where hekate looks for it')
    for offset, text in ((0x1C94, b'okay'), (0x3F34, UART_A.encode()), (0x3F54, UART_A.encode())):
        image[dtb + offset : dtb + offset + len(text) + 1] = text + b'\0'
    return bytes(image)


def bl33_env(ini: Path, entry_index: int = 1) -> bytes:
    """Return U-Boot's environment from the hekate entry (every ini key, then hekate's own), magic below it."""
    lines = [line for line in ini.read_text().splitlines()[1:] if '=' in line]
    lines += ['uart_port=1', f'autoboot={entry_index}', 'autoboot_list=0', 'loader_rev=7']
    blob = bytearray(0x2000)
    struct.pack_into('<I', blob, 0xFFC, BL33_ENV_MAGIC)
    text = ''.join(line + '\n' for line in lines).encode()
    blob[0x1000 : 0x1000 + len(text)] = text
    return bytes(blob)


BL33_ENV_BLOB = BL33_ENV - 0x1000


def fuse_page() -> bytes:
    """Return the fuse cache as an OLED (Aula, T210B01) reads: ODM4 hardware type 4, ODM28's retail PMIC bit, an ID."""
    page = bytearray(0x1000)
    struct.pack_into('<I', page, 0x9D8, 4 << 16)
    struct.pack_into('<I', page, 0xA40, 1)
    struct.pack_into('<7I', page, 0xA00, 0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555, 0x66, 0x77)
    return bytes(page)
