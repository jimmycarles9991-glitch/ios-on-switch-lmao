#!/usr/bin/env python3
"""Assemble and sign TegraGM20BMetal.bundle, and write its trust-cache module.

The bundle half of the old build-tegra-gm20b-metal.py:

    bundle.py --xcode-env F --plugin <linked plugin> --nvk <libvulkan_nouveau.dylib> --stack <raw_stack.txt>
              --bundle <out dir> --trustcache <out> --trust-json <out>

Info.plist's SwitchShaderStack is the stack key the plugin is built with, which the shader seed checks its tools
against. The plugin and NVK are stripped (-x) and must be plain arm64 Mach-Os. NVK is signed first, then the bundle (so
the bundle's seal covers the final dylib), both ad hoc with 4 KiB pages (this kernel's page size) and SHA-256: no team,
no entitlements, no signing time, so the CDHashes are a function of the bytes. The trust cache is one version-2
module (vendor/unixnu/scripts/make-owner-trust-bundle.py): u32 version, uuid5 of the CDHashes, u32 count, then per
entry the 20-byte CDHash, hash type 2 (SHA-256), flags 0, constraint category 0, reserved 0, in ascending order.
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
    for name in ('xcode-env', 'plugin', 'nvk', 'stack', 'bundle', 'trustcache', 'trust-json'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    xcode = Xcode(args.xcode_env)

    bundle = Path(args.bundle)
    if bundle.exists():
        shutil.rmtree(bundle)
    bundle.mkdir(parents=True)
    exe = bundle / NAME
    shutil.copyfile(args.plugin, exe)
    exe.chmod(0o755)
    xcode.run('strip', '-x', exe)
    arm64(exe)

    dylib = bundle / 'libvulkan_nouveau.dylib'
    shutil.copyfile(args.nvk, dylib)
    dylib.chmod(0o755)
    xcode.run('strip', '-x', dylib)
    arm64(dylib)
    nvk_cdhash = sign(xcode, dylib, 'com.brunostjohn.switch.nvk')
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
        path.chmod(0o755 if path.is_dir() or path in (exe, dylib) else 0o644)

    Path(args.trustcache).write_bytes(trust_module([plugin_cdhash, nvk_cdhash]))
    Path(args.trust_json).write_text(
        json.dumps(
            {
                'plugin_cdhash': plugin_cdhash,
                'nvk_cdhash': nvk_cdhash,
                'plugin_bytes': exe.stat().st_size,
                'nvk_bytes': dylib.stat().st_size,
            },
            indent=2,
        )
        + '\n'
    )
    print(f'bundle: plugin {plugin_cdhash} nvk {nvk_cdhash}')


if __name__ == '__main__':
    main()
