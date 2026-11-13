"""Where a run's batches go: the Switch over USB (whichever root `doctor` found) or a fake device in a folder."""

import json
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

from tools.benchloop import fakedevice, loop, protocol, transport

STATE = Path.home() / 'Library' / 'Application Support' / 'switch-ios' / 'benchloop'
CHOICE = STATE / 'transport.json'


@dataclass
class Session:
    transport: object
    stream: loop.LogStream | None = None

    def run(self, b: dict, into: Path) -> loop.Outcome:
        return loop.run(self.transport, b, into, self.stream)

    def run_all(self, experiments: list[dict], name: str, into: Path) -> dict[str, dict]:
        """`experiments` in batches of at most protocol.MAX_EXPERIMENTS; every result by id."""
        results: dict[str, dict] = {}
        step = protocol.MAX_EXPERIMENTS
        for start in range(0, len(experiments), step):
            part = experiments[start : start + step]
            outcome = self.run(protocol.batch(unique(f'{name}-{start // step}'), part), into)
            results |= outcome.results
            if not outcome.complete:
                missing = [e['id'] for e in part if e['id'] not in outcome.results]
                print(f'{outcome.batch}: no done.json after {outcome.seconds:.0f} s; {len(missing)} results missing')
        return results

    def close(self) -> None:
        if self.stream:
            self.stream.close()


def unique(name: str) -> str:
    """Give `name` with the time, so a batch never meets an earlier batch's results on the device."""
    return f'{name}-{time.strftime("%Y%m%d-%H%M%S")}-{time.monotonic_ns() % 1000000:06d}'


def fake(folder: Path) -> Session:
    device = fakedevice.FakeDevice(folder)
    return Session(transport.Local(folder, device.drain))


def usb(udid: str | None) -> Session:
    choice = json.loads(CHOICE.read_text()) if CHOICE.is_file() else {}
    afc = transport.Afc(udid=udid or choice.get('udid'), documents=choice.get('documents'))
    try:
        stream = loop.LogStream(afc.udid)
    except OSError:
        stream = None
    return Session(afc, stream)


def remember(afc: transport.Afc) -> None:
    STATE.mkdir(parents=True, exist_ok=True)
    CHOICE.write_text(json.dumps({'udid': afc.udid, 'documents': afc.documents}, indent=1) + '\n')


def paired(udid: str | None) -> str:
    command = ['ideviceinfo', '-k', 'ProductVersion'] + (['-u', udid] if udid else [])
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired) as e:
        return f'ideviceinfo: {e}'
    return (done.stdout or done.stderr).strip()
