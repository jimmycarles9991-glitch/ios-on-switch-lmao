"""What is plugged into the emulated USB-C port (README.md, USB host).

Each device is a QEMU USB device on the XUSB host's bus (xusb.0); the first sits on the connector (port 1) and, when it
is a hub, the rest sit behind it. A device on the port is also a sink-only partner on CC, so the BM92T36 sees one and
the driver sources VBUS to it; the device shows to the xHCI only once that VBUS is on. Each device's traffic lands in a
pcap in the run directory, and the xHCI traces every non-zero interrupter target into qemu-guest.log.
"""

import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

BUS = 'xusb.0'
CONNECTOR = '1'
STICK_BYTES = 64 << 20
TRACES = ('usb_xhci_intr_target', 'usb_xhci_slot_intr_target')


@dataclass(frozen=True)
class Device:
    driver: str
    product: str  # its USB product string, as Apple's stack names it when it enumerates the device


DEVICES = {
    'keyboard': Device('usb-kbd', 'QEMU USB Keyboard'),
    'storage': Device('usb-storage', 'QEMU USB HARDDRIVE'),
    'hub': Device('usb-hub', 'QEMU USB Hub'),
}


def ports(names: list[str]) -> list[str]:
    """Return each device's USB port path: the first on the connector, the rest behind it, which must be a hub."""
    if len(names) > 1 and names[0] != 'hub':
        raise SystemExit(
            '--usb: a second device needs a hub first (--usb hub --usb keyboard): the port is one connector'
        )
    return [CONNECTOR] + [f'{CONNECTOR}.{i}' for i in range(1, len(names))]


def pcap(index: int, name: str) -> str:
    return f'usb{index}-{name}.pcap'


def make_stick(image: Path) -> None:
    if not shutil.which('mformat'):
        raise SystemExit('--usb storage needs mtools: brew install mtools')
    with image.open('wb') as f:
        f.truncate(STICK_BYTES)
    subprocess.run(['mformat', '-i', str(image), '-v', 'SWITCHUSB', '::'], check=True, capture_output=True)


def qemu_args(run: Path, names: list[str]) -> list[str]:
    """Return the QEMU arguments that plug `names` (DEVICES' keys) into the port; a stick gets its own image."""
    if not names:
        return []
    args = ['-global', 'bm92t36.partner=sink']
    for trace in TRACES:
        args += ['-trace', trace]
    for index, (name, port) in enumerate(zip(names, ports(names), strict=True)):
        props = [DEVICES[name].driver, f'bus={BUS}', f'port={port}', f'id=usb{index}', f'pcap={pcap(index, name)}']
        if name == 'storage':
            image = run / f'usb{index}-stick.img'
            make_stick(image)
            args += ['-drive', f'if=none,id=stick{index},file={image},format=raw']
            props.append(f'drive=stick{index}')
        args += ['-device', ','.join(props)]
    return args
