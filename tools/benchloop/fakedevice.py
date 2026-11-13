"""HW Test's bench loop played on the Mac over a folder, so the whole loop runs without a Switch.

The folder side is hw_test_app/kit/BenchLoop.swift's: batches from inbox/, a journal line before and after each
experiment, one result file each, done.json, the batch moved beside its results. The GPU side is fakegpu.py's model.
Result files have the app's fields (core/BenchResult.swift), so the host reads them with the same code.
"""

import json
import shutil
import time
import zlib
from pathlib import Path

from tools.benchloop import fakegpu, protocol

DISTINCT_LIMIT = 64


def distinct(data: bytes, width: int, size: int) -> list[dict]:
    seen: dict[bytes, dict] = {}
    for index in range(len(data) // size):
        texel = data[index * size : (index + 1) * size]
        if texel in seen:
            seen[texel]['count'] += 1
        elif len(seen) < DISTINCT_LIMIT:
            seen[texel] = {'value': texel.hex(), 'count': 1, 'first': [index % width, index // width]}
    return list(seen.values())


def texel_fields(e: dict, data: bytes) -> dict:
    fmt, size = e['target']['format'], protocol.FORMATS[e['target']['format']][1]
    fields = {
        'format': fmt,
        'width': e['target']['width'],
        'height': e['target']['height'],
        'crc32': f'{zlib.crc32(data):08x}',
        'distinct': distinct(data, e['target']['width'], size),
    }
    if e.get('readback') == 'full':
        fields['data'] = protocol.b64(data)
    return fields


def encode_result(batch: str, e: dict, outcome: fakegpu.Outcome) -> bytes:
    top = {
        'protocol': protocol.PROTOCOL,
        'batch': batch,
        'id': e.get('id', '-'),
        'kind': e.get('kind', '-'),
        'status': outcome.status,
        'why': outcome.why,
        'elapsed_ms': outcome.milliseconds,
        'runs': [{'draws': d, 'gpu_ns': g, 'wall_ns': w} for d, g, w in outcome.runs],
    }
    if outcome.texels is not None and e.get('readback', 'summary') != 'none':
        top['texels'] = texel_fields(e, outcome.texels)
    if outcome.output is not None and e.get('readback', 'summary') != 'none':
        top['output'] = {
            'bytes': len(outcome.output),
            'crc32': f'{zlib.crc32(outcome.output):08x}',
            'distinct': distinct(outcome.output, len(outcome.output) // 4, 4),
        }
        if e.get('readback') == 'full':
            top['output']['data'] = protocol.b64(outcome.output)
    return (json.dumps(top, sort_keys=True, separators=(',', ':')) + '\n').encode()


def journalled(text: str) -> tuple[set[str], set[str]]:
    """Give the ids started and never done, and the ids done (core/BenchResult.swift BenchJournal)."""
    started, done = [], set()
    for words in (line.split(' ') for line in text.splitlines()):
        if len(words) >= 2 and words[0] == 'starting':
            started.append(words[1])
        elif len(words) >= 2 and words[0] == 'done':
            done.add(words[1])
    return set(started) - done, done


class FakeDevice:
    def __init__(self, root: Path, gpu: fakegpu.Gpu | None = None) -> None:
        self.root = root
        self.gpu = gpu or fakegpu.Gpu()
        for folder in ('inbox', 'outbox'):
            (root / folder).mkdir(parents=True, exist_ok=True)
        status = {'protocol': protocol.PROTOCOL, 'device': 'fake', 'selfcheck': 'PASS fake', 'root': str(root)}
        (root / 'status.json').write_text(json.dumps(status, sort_keys=True) + '\n')
        self.log: list[str] = []

    def drain(self) -> None:
        for path in sorted((self.root / 'inbox').glob('*.json')):
            if not path.name.startswith('.'):
                self.process(path)

    def process(self, path: Path) -> None:
        try:
            b = json.loads(path.read_bytes())
            if b.get('protocol') != protocol.PROTOCOL or not protocol.NAME.match(b.get('batch', '')):
                raise protocol.InvalidError('protocol or batch name')
        except (ValueError, protocol.InvalidError) as e:
            self.finish(path, path.stem, {'why': str(e)})
            return
        name = b['batch']
        outbox = self.root / 'outbox' / name
        if (outbox / 'done.json').is_file():
            shutil.rmtree(outbox)
        outbox.mkdir(parents=True, exist_ok=True)
        journal = outbox / 'journal.txt'
        crashed, finished = journalled(journal.read_text() if journal.is_file() else '')
        counts: dict[str, int] = {}
        for e in b['experiments']:
            if e.get('id') in finished:
                continue
            if e.get('id') in crashed:
                outcome = fakegpu.Outcome('crashed', "the app's process ended while it ran")
            else:
                with journal.open('a') as j:
                    j.write(f'starting {e.get("id")}\n')
                outcome = self.run(e)
            (outbox / f'{e.get("id")}.json').write_bytes(encode_result(name, e, outcome))
            with journal.open('a') as j:
                j.write(f'done {e.get("id")} {outcome.status}\n')
            counts[outcome.status] = counts.get(outcome.status, 0) + 1
            self.log.append(f'BENCH {name} {e.get("id")} {outcome.status}')
        self.finish(path, name, {'experiments': len(b['experiments']), 'statuses': counts})

    def run(self, e: dict) -> fakegpu.Outcome:
        started = time.monotonic()
        try:
            protocol.validate_experiment(e)
        except (protocol.InvalidError, KeyError, TypeError, ValueError) as error:
            return fakegpu.Outcome('invalid', str(error))
        outcome = self.gpu.run(e)
        outcome.milliseconds = round((time.monotonic() - started) * 1e3, 3)
        return outcome

    def finish(self, path: Path, name: str, summary: dict) -> None:
        outbox = self.root / 'outbox' / name
        outbox.mkdir(parents=True, exist_ok=True)
        done = summary | {'protocol': protocol.PROTOCOL, 'batch': name, 'device': 'fake'}
        (outbox / 'done.json').write_text(json.dumps(done, sort_keys=True) + '\n')
        path.replace(outbox / 'batch.json')
        self.log.append(f'BENCH {name} done')
