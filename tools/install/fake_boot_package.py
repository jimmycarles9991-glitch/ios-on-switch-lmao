#!/usr/bin/env python3
"""fake_boot_package.py <out dir> [--generation N]: a small stand-in for //:boot (same layout, dummy bytes).

For the installer's and deploy's tests only: switchroot/ios/{bl31,bl33}.bin, bootloader/ini/ios.ini (the real
entry's text, //boot:ios_ini), overlay/<two files>. --generation changes bl33.bin's and the overlay's contents, to test
that a rerun rewrites exactly the files that differ.
"""

import sys
from pathlib import Path

FILES = ['bl31.bin', 'bl33.bin']
INI = (
    '[iOS]\nl4t=1\nboot_prefixes=/switchroot/ios/\nid=iOS\nicon=bootloader/res/icon_ios_hue.bmp\n'
    'logopath=switchroot/ios/bootlogo.bmp\nbootwait=0\n'
    'scan_dev_for_extlinux=setexpr.l ios_uboot *0x60005010\nbootcmd_mmc0=true\n'
)


def gpu_bundle(out: Path) -> None:
    """//gpu:bundle's layout with dummy files (deploy --stage-gpu tests)."""
    bundle = out / 'ngpu/addons/TegraGM20BMetal.bundle'
    bundle.mkdir(parents=True)
    for name in ('TegraGM20BMetal', 'Info.plist'):
        (bundle / name).write_bytes(b'fake ' + name.encode())
    replies = out / 'ngpu/addons/TegraGM20BMetal.seed/shared/msl'
    replies.mkdir(parents=True)
    (replies / ('0' * 64 + '.reply')).write_bytes(b'fake reply')
    (out / 'TegraGM20BMetal.trustcache').write_bytes(bytes(24))


def main() -> None:
    out = Path(sys.argv[1])
    if '--gpu' in sys.argv:
        gpu_bundle(out)
        return
    generation = int(sys.argv[sys.argv.index('--generation') + 1]) if '--generation' in sys.argv else 0
    for name in FILES:
        path = out / 'switchroot/ios' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        body = f'fake {name} for the installer test'.encode()
        if name == 'bl33.bin':
            body += b' generation %d' % generation
        path.write_bytes(body * 64)
    ini = out / 'bootloader/ini/ios.ini'
    ini.parent.mkdir(parents=True, exist_ok=True)
    ini.write_text(INI)
    for relative, mode in (
        ('usr/sbin/BlueTool', 0o755),
        ('System/Library/LaunchDaemons/com.brunostjohn.switch.test.plist', 0o644),
    ):
        path = out / 'overlay' / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'overlay ' + relative.encode() + b' generation %d\n' % generation)
        path.chmod(mode)


if __name__ == '__main__':
    main()
