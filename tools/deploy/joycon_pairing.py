"""Joy-Con pairing from hekate's "Dump Joy-Con BT", staged on Data so the Joy-Cons paired in Horizon reconnect.

hekate (nyx gui_options.c _joycon_info_dump_action) reads the pairing slot in use of each rail Joy-Con and writes
switchroot/joycon_mac.ini on the FAT, one section per Joy-Con:

    [joycon_00]                         joycon_00 is the left rail's, joycon_01 the right's
    type=1                              1 Joy-Con (L), 2 Joy-Con (R); 0 when none or paired to a PC, not Horizon
    mac=AA:BB:CC:DD:EE:FF               the Joy-Con's BD address
    host=98:B6:E9:11:22:33              the address it pairs to: Horizon's for this console
    ltk=00112233445566778899AABBCCDDEEFF   the link key, in the order HCI and BlueZ take it (hekate reverses the slot's)

switchroot uses it as is: U-Boot sets the controller's address to `host` (boot.txt, from joycon_mac.bin) and the
BlueZ import writes `ltk` as the link key (Lakka switch-bsp pair-joycon.sh: Key=ltk, Type=4). Here the deploy tool
reads it, read-only, and stages two files under <Data>/Switch/bluetooth/ (/private/var/Switch/bluetooth/):

    host-address            6 bytes, display order: SwitchBluetooth programs it into the controller at bluetoothd's
                            first connect (drivers/SwitchBluetooth/SwitchBluetooth.cpp)
    joycon-pairing.plist    {HostAddress, Source, Controllers: [{Address, Name, ProductID, LinkKey (16 bytes)}]}: what
                            joyconpaird hands to bluetoothd at boot (drivers/SwitchJoyCon/helper/joyconpaird.c)

No dump on the FAT removes both, and the console goes back to its own address. Nothing is ever written to the FAT.
"""

import plistlib
import re
from collections.abc import Callable
from pathlib import Path

from tools.common import files

INI = 'switchroot/joycon_mac.ini'
STAGED = 'Switch/bluetooth'
PRODUCTS = {1: ('Joy-Con (L)', 0x2006), 2: ('Joy-Con (R)', 0x2007), 3: ('Pro Controller', 0x2009)}
_ADDRESS = re.compile(r'^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$')


def _sections(text: str) -> dict[str, dict[str, str]]:
    """Return the ini text's sections, each its keys (lower case) and values."""
    sections, current = {}, None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith('[') and line.endswith(']'):
            current = sections.setdefault(line[1:-1].strip(), {})
        elif current is not None and '=' in line:
            key, value = line.split('=', 1)
            current[key.strip().lower()] = value.strip()
    return sections


def _unusable(name: str, entry: dict[str, str], kind: int) -> str | None:
    """Return why a controller entry cannot be staged, or None."""
    mac, peer, ltk = entry.get('mac', ''), entry.get('host', ''), entry.get('ltk', '')
    if kind not in PRODUCTS:
        return '{}: type {} (none, or not paired in Horizon)'.format(name, entry.get('type'))
    if not _ADDRESS.match(mac) or not _ADDRESS.match(peer) or not re.fullmatch(r'[0-9A-Fa-f]{32}', ltk):
        return f'{name}: malformed mac/host/ltk'
    if not any(bytes.fromhex(ltk)) or peer.replace(':', '') == '000000000000':
        return f'{name}: empty host or key'
    return None


def parse(text: str) -> dict | None:
    """Parse joycon_mac.ini text: {"host": "98:b6:e9:..", "controllers": [...], "skipped": [reasons]}.

    Returns None when the text has no usable entry.
    """
    sections = _sections(text)
    controllers, skipped, host = [], [], None
    for name in sorted(n for n in sections if n.startswith('joycon_')):
        entry = sections[name]
        try:
            kind = int(entry.get('type', '0'), 0)
        except ValueError:
            kind = 0
        reason = _unusable(name, entry, kind)
        if reason:
            skipped.append(reason)
            continue
        peer = entry['host'].lower()
        host = host or peer
        if peer != host:
            # One controller address only (switchroot takes one as well): this Joy-Con was paired to another console.
            skipped.append(f'{name}: paired to {peer}, not {host}')
            continue
        label, product = PRODUCTS[kind]
        controllers.append(
            {
                'Address': entry['mac'].lower(),
                'Name': label,
                'ProductID': product,
                'LinkKey': bytes.fromhex(entry['ltk']),
            }
        )
    if not controllers:
        return {'host': None, 'controllers': [], 'skipped': skipped} if skipped else None
    return {'host': host, 'controllers': controllers, 'skipped': skipped}


def read(fat_root: Path, log: Callable[..., None]) -> dict | None:
    """Return the dump on a mounted FAT, parsed; None when there is none. Only reads."""
    path = Path(fat_root) / INI
    if not path.is_file():
        log(f'joycon pairing: no {INI} on the FAT (hekate: Tools > Dump Joy-Con BT, after pairing in Horizon)')
        return None
    pairing = parse(path.read_text(errors='replace'))
    if pairing is None:
        log(f'joycon pairing: {INI} has no Joy-Con sections')
    for reason in (pairing or {}).get('skipped', []):
        log('joycon pairing: skipped', reason)
    if pairing and pairing['controllers']:
        log(
            'joycon pairing: host {}, {}'.format(
                pairing['host'], ', '.join('{} {}'.format(c['Name'], c['Address']) for c in pairing['controllers'])
            )
        )
    return pairing if pairing and pairing['controllers'] else None


def stage(data_root: Path, pairing: dict | None, log: Callable[..., None], dry_run: bool = False) -> bool:
    """Stage the dump as <Data>/Switch/bluetooth/{host-address,joycon-pairing.plist}, or remove them without one.

    Returns whether anything changed.
    """
    directory = Path(data_root) / STAGED
    address, plist = directory / 'host-address', directory / 'joycon-pairing.plist'
    if not pairing:
        stale = [p for p in (address, plist) if p.exists()]
        if stale and not dry_run:
            for path in stale:
                path.unlink()
        if stale:
            log(
                'joycon pairing: staged dump removed{} (the console uses its own address again)'.format(
                    ' (dry run)' if dry_run else ''
                )
            )
        return bool(stale)
    wanted = {
        address: bytes.fromhex(pairing['host'].replace(':', '')),
        plist: plistlib.dumps(
            {'HostAddress': pairing['host'], 'Source': INI, 'Controllers': pairing['controllers']},
            fmt=plistlib.FMT_XML,
            sort_keys=True,
        ),
    }
    changed = [p for p, content in wanted.items() if not p.is_file() or p.read_bytes() != content]
    if changed and not dry_run:
        directory.mkdir(parents=True, exist_ok=True)
        directory.chmod(0o755)
        for path in changed:
            files.write_bytes(path, wanted[path], mode=0o644)
    log(
        'joycon pairing: {}{}'.format(
            'staged {} for {}'.format('/'.join(p.name for p in changed), pairing['host'])
            if changed
            else 'staged dump unchanged',
            ' (dry run)' if dry_run and changed else '',
        )
    )
    return bool(changed)
