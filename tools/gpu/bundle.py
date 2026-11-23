#!/usr/bin/env python3
"""Assemble and sign TegraGM20BMetal.bundle, and write its trust-cache module.

The bundle half of the old build-tegra-gm20b-metal.py:

    bundle.py --xcode-env F --plugin <linked plugin> --stack <stack_key.txt> --translate-header <NativeMetalTranslate.h>
              --bundle <out dir> --trustcache <out> --trust-json <out>

Info.plist's SwitchShaderStack is the stack key the plugin is built with, which the shader seed checks its tools
against. The linked plugin holds no symbol of the Metal-to-Vulkan fallback (m2v_symbols); it is stripped (-x) and must
be a plain arm64 Mach-O. The bundle is signed ad hoc with 4 KiB pages (this kernel's page size) and SHA-256: no team, no
entitlements, no signing time, so the CDHash is a function of the bytes. The trust cache is one version-2 module
(vendor/unixnu/scripts/make-owner-trust-bundle.py): u32 version, uuid5 of the CDHashes, u32 count, then per entry the
20-byte CDHash, hash type 2 (SHA-256), flags 0, constraint category 0, reserved 0, in ascending order.
"""

import argparse
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

from tools.common import macho
from tools.trustcache import trustcache

NAME = 'TegraGM20BMetal'
# m2v's runtime crate and the bridge by their Rust crates' names (the linked plugin's local symbols), and the frontend's
# entry points and NB classes
M2V = re.compile(r'metal2vulkan_runtime|native_metal_bridge|^_native_bridge_metal_|^_OBJC_(?:META)?CLASS_\$_NB[A-Z]')
NMB = re.compile(r'^_(nmb_\w+)$')


class Xcode:
    def __init__(self, env_file: str):
        values = dict(line.split('=', 1) for line in Path(env_file).read_text().splitlines() if '=' in line)
        self.toolchain = values['XCODE_TOOLCHAIN']
        self.env = {
            'PATH': f'{self.toolchain}:/usr/bin:/bin',
            'DEVELOPER_DIR': values['DEVELOPER_DIR'],
            'TMPDIR': os.environ.get('TMPDIR', '/tmp'),
        }

    def run(self, *command: str | Path) -> str:
        tool = command[0]
        path = Path(self.toolchain, tool) if Path(self.toolchain, tool).is_file() else Path('/usr/bin', tool)
        result = subprocess.run(
            [str(path), *map(str, command[1:])], env=self.env, capture_output=True, text=True, check=False
        )
        if result.returncode:
            sys.exit(f'FAILED: {" ".join(map(str, command))}\n{result.stdout}{result.stderr}')
        return result.stdout + result.stderr


def arm64(path: Path) -> None:
    try:
        plain = macho.macho(Path(path).read_bytes(), subtypes=None)['cpusubtype'] & 0xFFFFFF == 0
    except ValueError:
        plain = False
    if not plain:
        sys.exit(f'{path}: expected a plain arm64 (subtype 0) Mach-O')


def translation_entries(header: Path) -> set[str]:
    """Give the nmb_ functions the translation's header declares, its comments left out."""
    code = re.sub(r'//[^\n]*|/\*.*?\*/', '', header.read_text(), flags=re.DOTALL)
    return set(re.findall(r'\b(nmb_\w+)\s*\(', code))


def m2v_symbols(symbols: list[str], translation: set[str]) -> list[str]:
    """Give the symbols of m2v's runtime, the bridge or the frontend.

    An nmb_ function the translation does not declare is the bridge's.
    """
    return sorted({s for s in symbols if M2V.search(s) or ((m := NMB.match(s)) and m[1] not in translation)})


def sign(xcode: Xcode, path: Path, identifier: str) -> str:
    xcode.run('codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', identifier, path)
    xcode.run('codesign', '--verify', '--strict', path)
    detail = xcode.run('codesign', '--display', '--verbose=4', path)
    if not re.search(r'^Page size=4096$', detail, re.MULTILINE) or not re.search(
        r'^Hash type=sha256', detail, re.MULTILINE
    ):
        sys.exit(f'{path}: expected a SHA-256 signature with 4 KiB pages')
    return re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.MULTILINE)[1]


def trust_module(cdhashes: list[str]) -> bytes:
    hashes = sorted({bytes.fromhex(h) for h in cdhashes})
    identifier = uuid.uuid5(
        uuid.NAMESPACE_URL, 'com.brunostjohn.switch.tegra-gm20b-metal:' + ''.join(h.hex() for h in hashes)
    )
    return trustcache.trust_module(hashes, uuid=identifier.bytes)


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ('xcode-env', 'plugin', 'stack', 'translate-header', 'bundle', 'trustcache', 'trust-json'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    xcode = Xcode(args.xcode_env)
    symbols = xcode.run('nm', '-U', '-j', args.plugin).split()
    held = m2v_symbols(symbols, translation_entries(Path(args.translate_header)))
    if held:
        sys.exit(f'{args.plugin}: holds {len(held)} symbols of the Metal-to-Vulkan fallback: {", ".join(held[:8])}')

    bundle = Path(args.bundle)
    if bundle.exists():
        shutil.rmtree(bundle)
    bundle.mkdir(parents=True)
    exe = bundle / NAME
    shutil.copyfile(args.plugin, exe)
    exe.chmod(0o755)
    xcode.run('strip', '-x', exe)
    arm64(exe)
    (bundle / 'Info.plist').write_bytes(
        plistlib.dumps(
            {
                'CFBundleIdentifier': 'com.brunostjohn.switch.' + NAME,
                'CFBundleExecutable': NAME,
                'CFBundlePackageType': 'BNDL',
                'CFBundleVersion': '1',
                'CFBundleShortVersionString': '1',
                'NSPrincipalClass': 'TegraGM20BMetalDevice',
                'MinimumOSVersion': '17.0',
                'SwitchShaderStack': Path(args.stack).read_text().strip(),
            },
            fmt=plistlib.FMT_XML,
        )
    )
    plugin_cdhash = sign(xcode, bundle, 'com.brunostjohn.switch.' + NAME)
    for path in [bundle, *bundle.rglob('*')]:
        path.chmod(0o755 if path.is_dir() or path == exe else 0o644)

    Path(args.trustcache).write_bytes(trust_module([plugin_cdhash]))
    Path(args.trust_json).write_text(
        json.dumps({'plugin_cdhash': plugin_cdhash, 'plugin_bytes': exe.stat().st_size}, indent=2) + '\n'
    )
    print(f'bundle: plugin {plugin_cdhash}')


if __name__ == '__main__':
    main()
