"""One batch through the loop: written to the device's inbox, waited for, its results read back.

The wait follows the app's `BENCH` log lines (idevicesyslog) when a stream is given: the `BENCH <batch> done` line ends
it, and each experiment's line is kept as the fallback return path for when the outbox cannot be read. The kernel's GR
error lines in the same stream are given to the experiment whose `BENCH` line follows them, since the app logs an
experiment only after its channel died. With no stream it asks for the batch's done.json every few seconds.
"""

import queue
import subprocess
import tempfile
import threading
import time
from collections.abc import Callable, Iterator
from dataclasses import dataclass, field
from pathlib import Path

from tools.benchloop import gr_errors, protocol

POLL_S = 2.0
SLACK_S = 60.0


class LogStream:
    """`idevicesyslog` lines that carry `BENCH ` or a kernel GR error, read on a thread so a wait can time out."""

    def __init__(self, udid: str | None, tool: str = 'idevicesyslog') -> None:
        command = [tool, '--no-colors', '-m', 'BENCH ', '-m', 'gr: ']
        if udid:
            command[1:1] = ['-u', udid]
        self._process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self._lines: queue.Queue[str] = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        for line in self._process.stdout or ():
            self._lines.put(line.rstrip('\n'))

    def lines(self, until: float) -> Iterator[str]:
        while (left := until - time.monotonic()) > 0:
            try:
                yield self._lines.get(timeout=min(left, 1.0))
            except queue.Empty:
                continue

    def close(self) -> None:
        self._process.terminate()


@dataclass
class Outcome:
    batch: str
    results: dict[str, dict] = field(default_factory=dict)
    log: dict[str, dict] = field(default_factory=dict)
    kernel: dict[str, list[str]] = field(default_factory=dict)
    done: dict | None = None
    seconds: float = 0.0

    @property
    def complete(self) -> bool:
        return self.done is not None


def budget_s(b: dict) -> float:
    """Give a generous bound on the device's time for `b`: every command buffer at its estimate plus fixed costs."""
    total = SLACK_S
    for e in b['experiments']:
        buffers = len(e.get('draws', [1])) * (e.get('repeats', 1) + e.get('warmup', 0))
        total += 0.2 + buffers * (protocol.estimated_ms(e) + 5.0) / 1e3
    return total


def parse_log_line(line: str) -> tuple[str, str, dict] | None:
    """`BENCH <batch> <id> <status> runs=<draws>:<ns>,... crc=<hex> ms=<ms> [why=...]` from BenchResult.logLine."""
    at = line.find('BENCH ')
    if at < 0:
        return None
    words = line[at:].split(' ')
    if len(words) < 4 or words[3] not in protocol.STATUSES:
        return None
    fields = dict(w.split('=', 1) for w in words[4:] if '=' in w)
    runs = []
    for run in filter(None, fields.get('runs', '').split(',')):
        draws, ns = run.split(':')
        runs.append({'draws': int(draws), 'gpu_ns': [int(ns)], 'wall_ns': []})
    result = {'protocol': protocol.PROTOCOL, 'id': words[2], 'status': words[3], 'runs': runs, 'from': 'log'}
    if 'crc' in fields:
        result['texels'] = {'crc32': fields['crc']}
    if 'why' in fields:
        result['why'] = fields['why']
    return words[1], words[2], result


def push(transport: object, b: dict) -> None:
    name = b['batch']
    inbox = f'{transport.root}/inbox'
    transport.mkdir(inbox)
    transport.mkdir(f'{transport.root}/outbox')
    with tempfile.TemporaryDirectory() as scratch:
        local = Path(scratch) / f'{name}.json'
        local.write_bytes(protocol.encode_batch(b))
        transport.put(local, f'{inbox}/.{name}.json.part')
    transport.mv(f'{inbox}/.{name}.json.part', f'{inbox}/{name}.json')


def wait(transport: object, b: dict, stream: LogStream | None, sleep: Callable[[float], None] = time.sleep) -> Outcome:
    name, started = b['batch'], time.monotonic()
    until = started + budget_s(b)
    outcome = Outcome(name)
    outbox = f'{transport.root}/outbox/{name}'
    if stream:
        pending: list[str] = []
        for line in stream.lines(until):
            if 'BENCH ' not in line and gr_errors.is_gr_line(line):
                if (text := gr_errors.decode(line)) and text not in pending:
                    pending.append(text)
                continue
            parsed = parse_log_line(line)
            if parsed and parsed[0] == name:
                outcome.log[parsed[1]] = parsed[2]
                if pending:
                    outcome.kernel[parsed[1]], pending = pending, []
            if f'BENCH {name} done' in line:
                break
    else:
        while time.monotonic() < until and 'done.json' not in (transport.ls(outbox) or []):
            sleep(POLL_S)
    outcome.seconds = time.monotonic() - started
    return outcome


def pull(transport: object, outcome: Outcome, into: Path) -> Outcome:
    import json  # noqa: PLC0415  only here

    folder = into / outcome.batch
    try:
        transport.get(f'{transport.root}/outbox/{outcome.batch}', folder)
    except (OSError, RuntimeError):
        return outcome
    for path in sorted(folder.glob('*.json')):
        if path.name == 'done.json':
            outcome.done = json.loads(path.read_text())
        elif path.name != 'batch.json':
            try:
                result = protocol.decode_result(path.read_bytes())
            except (ValueError, protocol.InvalidError):
                continue
            outcome.results[result['id']] = result
    return outcome


def run(transport: object, b: dict, into: Path, stream: LogStream | None = None) -> Outcome:
    push(transport, b)
    outcome = pull(transport, wait(transport, b, stream), into)
    if outcome.complete:
        transport.rm(f'{transport.root}/outbox/{outcome.batch}')
    for experiment_id, result in outcome.log.items():
        outcome.results.setdefault(experiment_id, result)
    for experiment_id, lines in outcome.kernel.items():
        if experiment_id in outcome.results:
            outcome.results[experiment_id]['kernel'] = lines
    return outcome
