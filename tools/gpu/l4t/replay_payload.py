"""Build immutable per-state HW Test inputs outside the checkout."""

import argparse
import hashlib
import json
import os
import re
import struct
from pathlib import Path

from replay_inputs import SEEDS, Reflection, fixture

CASES = ('compositor', 'ae6d01e7cac0d964', '17d6ade58fef2152', '357f8355b68acebd', '3f9f7f04b2b91507')


def words(*values: int) -> bytes:
    return struct.pack(f'<{len(values)}I', *values)


def blob(data: bytes) -> bytes:
    return words(len(data)) + data


def program(directory: Path, stage: str) -> bytes:
    sph = (directory / f'{stage}.sph').read_bytes()
    code = (directory / f'{stage}.sass').read_bytes()
    constants = (directory / f'{stage}.constants').read_bytes()
    report = (directory / 'statistics.txt').read_text()
    stage_id = {'vert': 1, 'frag': 16}[stage]
    section = re.search(rf'^stage={stage_id} .*?(?=^stage=|\Z)', report, re.MULTILINE | re.DOTALL)
    fields = dict(re.findall(r'^  (Register Count|Binary Size) = (\d+)', section[0], re.MULTILINE)) if section else {}
    if len(sph) != 80 or not code or len(code) % 32 or int(fields.get('Binary Size', 0)) != len(sph) + len(code):
        raise ValueError(f'incomplete stage {directory}/{stage}')
    return words(int(fields['Register Count'])) + blob(sph + code) + blob(constants)


def verify_stages(profile: Path, records: Path) -> None:
    for i in range(2):
        stage = f'stage{i}'
        source = (profile / 'translated/0000' / (stage + '.r32.spv')).read_bytes()
        if hashlib.sha256(source).hexdigest() != (profile / 'nvidia' / (stage + '.sha256')).read_text().strip():
            raise ValueError(f'{profile.name}: NVIDIA received a different {stage}')
        if source != (records / 'stages/0000' / (stage + '.used.spv')).read_bytes():
            raise ValueError(f'{profile.name}: paired NAK received a different {stage}')


def payload(profile: Path, records: Path, case: str) -> tuple[str, bytearray]:
    profile = profile / case
    records = records / case
    (path,) = records.glob('*.rec')
    raw = json.loads(path.with_suffix('.json').read_bytes())
    paired = json.loads((profile / 'paired' / path.with_suffix('.json').name).read_bytes())
    for field in ('vs', 'fs', 'entries', 'blob', 'fs_gprs', 'fs_data'):
        if raw[field] != paired[field]:
            raise ValueError(f'{case}: different paired {field}')
    if raw['vs_gprs'] != json.loads((profile / 'vertices.json').read_bytes())['paired']['gprs']:
        raise ValueError('vertex register mismatch')
    verify_stages(profile, records)
    reflection = Reflection(path.with_suffix('.reflection').read_bytes())
    pso = json.loads((profile / 'translated/0000/pipeline.json').read_bytes())
    active = {(e[1], e[2]) for e in raw['entries'] if e[0] == 1}
    active.update(k for k, n in reflection.buffers.items() if reflection.name(n) == 'vertexBuffer.0')
    colors = [f for f in pso['color_formats'] if f]
    if colors != pso['color_formats'][: len(colors)] or len(pso['sets']) != 1:
        raise ValueError('unexpected layout')
    kinds = [b[1] for b in pso['sets'][0]['bindings']]
    if len(kinds) != len(raw['entries']):
        raise ValueError('descriptor order')
    out = bytearray(b'SWAB0001' + blob(path.stem.encode()) + blob(path.read_bytes()))
    out += words(pso['samples'], pso['depth_format'], len(colors), *colors) + blob(bytes(kinds))
    out += program(profile / 'nvidia', 'vert')
    out += program(profile / 'nvidia', 'frag')
    out += words(len(SEEDS))
    corpus = (profile / 'corpus.log').read_text()
    for i, seed in enumerate(SEEDS):
        blocks = fixture(reflection, active, case, corpus, i)
        out += words(seed, len(blocks))
        for stage, slot, offset, data in blocks:
            out += words(stage, slot, offset) + blob(data)
    return path.stem, out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('run', type=Path)
    ap.add_argument('destination', type=Path)
    ap.add_argument('--records', type=Path, help='paired directcheck output (default: RUN/bench-records)')
    args = ap.parse_args()
    if args.destination.resolve().is_relative_to(Path(__file__).resolve().parents[3]):
        ap.error('choose a destination outside the worktree')
    args.destination.mkdir(parents=True, exist_ok=True)
    for case in CASES:
        key, data = payload(args.run / 'profile', args.records or args.run / 'bench-records', case)
        path = args.destination / (key + '.swab')
        tmp = path.with_suffix('.tmp')
        with tmp.open('xb') as f:
            f.write(data)
        os.replace(tmp, path)
        print(key, hashlib.sha256(data).hexdigest(), len(data))


if __name__ == '__main__':
    main()
