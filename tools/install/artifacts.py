#!/usr/bin/env python3
"""//install:artifacts: everything the SD install writes, described without a device (a build, not a run).

    artifacts.py --spec spec.json --ipsw ipsw.json --boot-package <dir> --out <dir>

Writes into <dir>:
  bootloader/ini/ios.ini   the hekate entry (copied from the boot package, which owns it)
  uuid-spec.json                the partition type and name and the volume/cryptex UUIDs the kernel sources define
  preboot-staging.json          the Preboot files: path on Preboot, IPSW member, size, sha256, owner and mode
  data-seed-plan.txt            how Data is seeded and what the kernel itself creates on first boot
  boot-entry.json               switchroot/ios/* and the ini: size and sha256 of each file
  manifest.json                 all of the above in one record (IPSW identity, sizes, sha256s)
"""

import argparse
import json
import shutil
from pathlib import Path

from tools.common import hashing

DATA_PLAN = """\
Data volume ({name}, UUID {uuid}, role Data, case-sensitive, unencrypted) is seeded once, by //install:sd:
  * the System volume's /private/var copied whole (ditto --rsrc --extattr --acl --preserveHFSCompression), owners kept
    (the installer runs as root): mobile 501:501, staged_system_apps, db/, ... (about 11,000 entries, 470 MB);
  * mobile/Library/Preferences created 501:501 if the seed lacks it (the kernel's preference writer does not create
    parent directories);
  * the Data root 0:0 0755 (launchd's policy step checks it);
  * Switch/overlay = the boot package's overlay/ (also refreshed by //deploy:sd).
A rerun never touches an existing seed (mobile/ present) unless --reseed-data.
The kernel creates the rest itself at boot (XNU kern_exec.c, SwitchRawMount.h): db/com.apple.xpc.launchd/
disabled.plist, keybags/containers/dslocal directories, setup-skip preferences, Switch/system-app-migration,
Switch/portrait-native-*, Switch/panic-*.log.
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('spec', 'ipsw', 'boot-package', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    spec, pins = json.loads(Path(args.spec).read_text()), json.loads(Path(args.ipsw).read_text())
    package, out = Path(args.boot_package), Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    ini = package / 'bootloader/ini/ios.ini'
    (out / 'bootloader/ini').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ini, out / 'bootloader/ini/ios.ini')
    (out / 'uuid-spec.json').write_text(json.dumps(spec, indent=2, sort_keys=True) + '\n')
    staging = []
    for item in spec['cryptexes']:
        pin = pins['members'][item['member']]
        if Path(item['path']).name != Path(pin['member']).name:
            raise SystemExit('{}: kernel path {} vs IPSW member {}'.format(item['member'], item['path'], pin['member']))
        staging.append(
            {
                'preboot_path': item['path'],
                'ipsw_member': pin['member'],
                'size': pin['size'],
                'sha256': pin['content_sha256'],
                'volume_uuid': item['volume_uuid'],
                'owner': '0:0',
                'mode': '0644',
            }
        )
    (out / 'preboot-staging.json').write_text(json.dumps(staging, indent=2) + '\n')
    data = spec['volumes']['data']
    (out / 'data-seed-plan.txt').write_text(DATA_PLAN.format(name=data['name'], uuid=data['uuid']))
    entry = {}
    for path in [*sorted((package / 'switchroot/ios').iterdir()), ini]:
        entry[path.relative_to(package).as_posix()] = {'size': path.stat().st_size, 'sha256': hashing.sha256_file(path)}
    (out / 'boot-entry.json').write_text(json.dumps(entry, indent=2) + '\n')
    overlay = package / 'overlay'
    manifest = {
        'ipsw': {k: pins[k] for k in ('product', 'device_class', 'version', 'build', 'url', 'size')},
        'partition': {'type': spec['partition_type'], 'gpt_name': spec['partition_name']},
        'volumes': spec['volumes'],
        'os_image': {k: pins['members']['os'][k] for k in ('member', 'size', 'content_sha256')},
        'preboot': staging,
        'boot_entry': entry,
        'overlay_files': sorted(p.relative_to(overlay).as_posix() for p in overlay.rglob('*') if p.is_file())
        if overlay.is_dir()
        else [],
    }
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
