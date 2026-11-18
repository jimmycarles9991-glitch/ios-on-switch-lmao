import copy
import plistlib
import stat
import struct
import sys
import warnings
import zipfile
from collections.abc import Sequence
from pathlib import Path

ROOT = 'Payload/Example.app/'
INFO = {
    'CFBundleIdentifier': 'org.example.app',
    'CFBundleExecutable': 'Example',
    'CFBundlePackageType': 'APPL',
    'CFBundleDisplayName': 'Example App',
    'CFBundleShortVersionString': '1.2.3',
    'MinimumOSVersion': '17.0',
}


def executable(cpu: int = 0x0100000C, subtype: int = 0, encrypted: bool = False, platform: int = 2) -> bytes:
    commands = struct.pack('<6I', 0x32, 24, platform, 0x00110000, 0x00110000, 0)
    commands += struct.pack('<6I', 0x2C, 24, 0, 0, int(encrypted), 0)
    return struct.pack('<8I', 0xFEEDFACF, cpu, subtype, 2, 2, len(commands), 0, 0) + commands


def universal(wide: bool = False, overlap: bool = False) -> bytes:
    image = executable()
    offset = 8 if overlap else 16384
    header = struct.pack('>II', 0xCAFEBABF if wide else 0xCAFEBABE, 1)
    header += (
        struct.pack('>IIQQII', 0x0100000C, 0, offset, len(image), 14, 0)
        if wide
        else struct.pack('>IIIII', 0x0100000C, 0, offset, len(image), 14)
    )
    return header + bytes(max(0, offset - len(header))) + image


def write(
    directory: Path,
    name: str,
    info: dict | None = None,
    image: bytes | None = None,
    extra: Sequence[tuple[str | zipfile.ZipInfo, bytes]] = (),
) -> None:
    metadata = copy.deepcopy(INFO if info is None else info)
    with zipfile.ZipFile(directory / f'{name}.ipa', 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        if name != 'missing_binary':
            archive.writestr(ROOT + 'Example', executable() if image is None else image)
        if name != 'missing_plist':
            data = plistlib.dumps(metadata) if name != 'corrupt_plist' else b'not a plist'
            if name == 'large_info':
                data = b'x' * (1024 * 1024 + 1)
            archive.writestr(ROOT + 'Info.plist', data)
        for path, data in extra:
            archive.writestr(path, data)


def main() -> None:
    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)
    write(directory, 'valid')
    write(directory, 'arm64e', image=executable(subtype=2))
    write(directory, 'fat', image=universal())
    write(directory, 'fat64', image=universal(wide=True))
    write(directory, 'fat_overlap', image=universal(overlap=True))
    write(directory, 'encrypted', image=executable(encrypted=True))
    write(directory, 'simulator', image=executable(platform=7))
    write(directory, 'x86', image=executable(cpu=0x01000007))
    for name in ('missing_binary', 'missing_plist', 'corrupt_plist', 'large_info'):
        write(directory, name)
    write(directory, 'unsafe_path', extra=[('../escape', b'bad')])
    link = zipfile.ZipInfo(ROOT + 'Frameworks/Outside')
    link.create_system = 3
    link.external_attr = (stat.S_IFLNK | 0o777) << 16
    write(directory, 'escaping_link', extra=[(link, b'../../../../outside')])
    with warnings.catch_warnings():
        warnings.simplefilter('ignore', UserWarning)
        write(directory, 'duplicate', extra=[(ROOT + 'Example', executable())])
    write(directory, 'apple_id', info=INFO | {'CFBundleIdentifier': 'com.apple.Preferences'})
    write(directory, 'self_id', info=INFO | {'CFBundleIdentifier': 'com.brunostjohn.switch.installer'})
    write(directory, 'self_id_uppercase', info=INFO | {'CFBundleIdentifier': 'COM.BRUNOSTJOHN.SWITCH.INSTALLER'})
    write(directory, 'future_os', info=INFO | {'MinimumOSVersion': '99.0'})
    write(directory, 'invalid_version', info=INFO | {'MinimumOSVersion': '17..0'})
    write(directory, 'multiple_apps', extra=[('Payload/Other.app/Other', executable())])
    internal = zipfile.ZipInfo(ROOT + 'Frameworks/Library.framework/Current')
    internal.create_system = 3
    internal.external_attr = (stat.S_IFLNK | 0o777) << 16
    write(directory, 'internal_link', extra=[(internal, b'../Library.framework/A')])
    image = bytearray(executable())
    struct.pack_into('<I', image, 36, 0xFFFFFFFF)
    write(directory, 'bad_commands', image=bytes(image))
    write(
        directory,
        'nested_bundles',
        extra=[
            (
                ROOT + 'PlugIns/Widget.appex/Info.plist',
                plistlib.dumps({'CFBundleIdentifier': 'org.example.app.widget'}),
            ),
            (ROOT + 'Frameworks/Library.framework/Library', b'framework fixture'),
        ],
    )


if __name__ == '__main__':
    main()
