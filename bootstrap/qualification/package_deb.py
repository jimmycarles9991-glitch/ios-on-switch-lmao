import sys
from pathlib import Path

from bootstrap import packages


def main() -> None:
    image, filters, output = map(Path, sys.argv[1:])
    control = {}
    packages.add_file(
        control,
        'control',
        b'Package: com.brunostjohn.switch.hook-probe\nVersion: 1.0\nArchitecture: iphoneos-arm64\n'
        b'Maintainer: Switch\nSection: Development\nDepends: ellekit\n'
        b'Description: Optional ElleKit Foundation hook qualification\n',
    )
    files = {}
    directory = 'var/jb/usr/lib/TweakInject'
    packages.add_file(files, f'{directory}/SwitchHookProbe.dylib', image.read_bytes(), 0o755)
    packages.add_file(files, f'{directory}/SwitchHookProbe.plist', filters.read_bytes())
    packages.write_deb(output, control, files)


if __name__ == '__main__':
    main()
