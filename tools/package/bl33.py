#!/usr/bin/env python3
"""switchroot's U-Boot (bl33.bin) as this boot entry ships it.

Its default environment edited, the boot script, the loader, the exporter and the DTB in their slots past U-Boot
(loader_abi.SCRIPT_SLOT and the others), then the staged kernel window (boot/loader/stage.h).

    bl33.py patch BL33 OUT                                   the environment edit (once: it never reads the kernel)
    bl33.py stage --u-boot PATCHED --window W --constants C --script S --loader L --exporter E --dtb D --out OUT
                  [--loader-constants LC]                     a loader built elsewhere, for the window LC describes
"""

import argparse
import json
import struct
from pathlib import Path

from build_native_loader import stage_into_bl33
from loader_abi import DTB_SLOT, EXPORTER_RUN, EXPORTER_SLOT, LOADER_SLOT, SCRIPT_SLOT, STAGE_SOURCE

# U-Boot's built-in default environment in switchroot's bl33.bin (env/common.c default_environment, imported whole at
# start: a run of "name=value\0" ended by an empty string), rewritten in place to the same size so nothing else in the
# binary moves:
#   * the boot script comes from its slot in this file (loader_abi.SCRIPT_SLOT), never from the FAT.
#   * boot_targets: SD (mmc1) only, so U-Boot never falls back to the eMMC, even when it cannot read the SD card and
#     hekate's env (bootcmd_mmc0=true above) is never imported; bootcmd_mmc0 itself goes.
#   * two boot timing samples (boot/loader/stamps.h) in what runs before hekate's env: bootcmd's start (U-Boot
#     initialized) and mmc_boot once `mmc dev 1` has brought the SD card up.
#   * room for them: four variables nothing on this boot path reads (PXE, ramdisk and DHCP script addresses).
ENV_START = b'\0bootcmd=run distro_bootcmd\0'
ENV_EDITS = {
    b'bootcmd=run distro_bootcmd': b'bootcmd=setexpr.l ios_u_main *0x60005010; run distro_bootcmd',
    b'mmc_boot=if mmc dev ${devnum}; then setenv devtype mmc; run scan_dev_for_boot_part; fi': (
        b'mmc_boot=if mmc dev ${devnum}; then setexpr.l ios_u_sd *0x60005010; setenv devtype mmc; '
        b'run scan_dev_for_boot_part; fi'
    ),
    b'boot_targets=mmc1 mmc0 ': b'boot_targets=mmc1',
    # `part list -bootable` took about 1 s from the SD card up to hekate's env imported (U-Boot re-reads and re-checks
    # the GPT for every one of its 64 partition indexes). hekate's FAT is always partition 1: scan it directly (no part
    # list, no fstype probe: test -e and load mount it themselves).
    (
        b'scan_dev_for_boot_part=part list ${devtype} ${devnum} -bootable devplist; env exists devplist || '
        b'setenv devplist 1; for distro_bootpart in ${devplist}; do if fstype ${devtype} ${devnum}:${distro_bootpart} '
        b'bootfstype; then run scan_dev_for_boot; fi; done'
    ): b'scan_dev_for_boot_part=setenv distro_bootpart 1; run scan_dev_for_boot',
}
ENV_EDITS[
    b'scan_dev_for_scripts=for script in ${boot_scripts}; do if test -e ${devtype} ${devnum}:${distro_bootpart} '
    b'${prefix}${script}; then echo Found U-Boot script ${prefix}${script}; run boot_a_script; '
    b'echoe SCRIPT FAILED: continuing...; fi; done'
] = b'scan_dev_for_scripts=source %#x; echoe SCRIPT FAILED: continuing...' % SCRIPT_SLOT[0]
ENV_DROPS = (
    b'pxefile_addr_r=0x8ff00000',
    b'ramdisk_addr_r=0x92000000',
    b'boot_script_dhcp=boot.scr.uimg',
    b'bootcmd_mmc0=setenv devnum 0; run mmc_boot',
    b'boot_scripts=boot.scr',
    (
        b'boot_a_script=load ${devtype} ${devnum}:${distro_bootpart} ${scriptaddr} ${prefix}${script}; '
        b'source ${scriptaddr}'
    ),
)


def bl33_patch(bl33: bytes) -> bytes:
    """Switchroot's U-Boot with its default environment as ENV_EDITS / ENV_DROPS say; every other byte unchanged."""
    if bl33.count(ENV_START) != 1:
        raise SystemExit('bl33.bin: U-Boot default environment not found exactly once')
    start = bl33.index(ENV_START) + 1
    end = bl33.index(b'\0\0', start) + 1  # the empty string that ends it
    entries = bl33[start : end - 1].split(b'\0')
    if any(entries.count(key) != 1 for key in (*ENV_EDITS, *ENV_DROPS)):
        raise SystemExit('bl33.bin: U-Boot default environment is not the one this patch was written for')
    entries = [ENV_EDITS.get(e, e) for e in entries if e not in ENV_DROPS]
    block = b'\0'.join(entries) + b'\0\0'
    if len(block) > end + 1 - start:
        raise SystemExit('bl33.bin: patched U-Boot environment does not fit')
    return bl33[:start] + block.ljust(end + 1 - start, b'\0') + bl33[end + 1 :]


def check_loader_constants(built_for: dict, window: dict) -> None:
    """Refuse a window other than the one a loader built elsewhere (the release's) was compiled for."""
    differ = sorted(k for k in built_for.keys() | window.keys() if built_for.get(k) != window.get(k))
    if differ:
        raise SystemExit(
            f"the release's loader was built for another kernel window ({', '.join(differ)} differ): compose from "
            "the release's own source (MODULE.bazel's version), or build the loader with --//install:prebuilt=local"
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    step = parser.add_subparsers(dest='step', required=True)
    p = step.add_parser('patch')
    p.add_argument('bl33', type=Path)
    p.add_argument('out', type=Path)
    p = step.add_parser('stage')
    for name in ('u-boot', 'window', 'constants', 'script', 'loader', 'exporter', 'dtb', 'out'):
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('--loader-constants', type=Path)
    args = parser.parse_args()
    if args.step == 'patch':
        args.out.write_bytes(bl33_patch(args.bl33.read_bytes()))
        return
    window, constants = args.window.read_bytes(), json.loads(args.constants.read_text())
    if args.loader_constants:
        check_loader_constants(json.loads(args.loader_constants.read_text()), constants)
    if constants['SOURCE'] != STAGE_SOURCE or constants['PACKED_BYTES'] != len(window):
        raise SystemExit('loader was not built for the bl33-staged window')
    data_bytes, load, entry = struct.unpack_from('>3I', args.exporter.read_bytes(), 12)
    if load != EXPORTER_RUN[0] or entry != EXPORTER_RUN[0] or data_bytes > EXPORTER_RUN[1]:
        raise SystemExit('exporter.uimg does not load and run where loader_abi.EXPORTER_RUN says')
    slots = [
        (*slot, path.read_bytes())
        for slot, path in (
            (SCRIPT_SLOT, args.script),
            (LOADER_SLOT, args.loader),
            (EXPORTER_SLOT, args.exporter),
            (DTB_SLOT, args.dtb),
        )
    ]
    args.out.write_bytes(stage_into_bl33(args.u_boot.read_bytes(), window, constants['STREAM_BYTES'], slots))


if __name__ == '__main__':
    main()
