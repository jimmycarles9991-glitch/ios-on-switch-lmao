"""First contact: the Switch, a bench folder both sides reach, and the calibration batch through it.

The calibration batch times empty passes, runs the app's known-answer program and the host's encoding of the same
program, dispatches an EXIT kernel and a kernel that stores known words to its output buffer. The folder that worked
is remembered for every later run.
"""

import json
import struct
import tempfile
from pathlib import Path

from tools.benchloop import golden, known, loop, protocol, session, transport
from tools.benchloop.sm53 import program

FLOAT = {'format': 'rgba32float', 'width': 16, 'height': 16}
KNOWN_TEXEL = known.KNOWN_DATA_BYTES.hex()
OUTPUT_WORDS = (0xB0E7C0DE, 0x0BADF00D)
OUTPUT_THREADS = 32
OUTPUT_KERNEL = program.compute_out([], {8: OUTPUT_WORDS[0], 9: OUTPUT_WORDS[1]}, (8, 9))
# Its line is printed, its verdict does not decide the loop: S2R, STG and IADD.CC/.X have not yet run on GM20B
INFORMATIVE = ('output-kernel',)


def app_protocol(afc: transport.Afc) -> str | None:
    """Give the protocol the app watching `afc` speaks (its status.json's), None where it says none."""
    data = afc.read(f'{afc.root}/status.json')
    try:
        return json.loads(data).get('protocol') if data else None
    except ValueError:
        return None


def calibration() -> list[dict]:
    return [
        {'id': 'noop', 'kind': 'noop', 'target': FLOAT, 'draws': [0], 'repeats': 5, 'warmup': 1, 'readback': 'none'},
        {'id': 'known', 'kind': 'known', 'target': FLOAT, 'readback': 'summary'},
        {
            'id': 'host-known',
            'kind': 'fragment',
            'target': FLOAT,
            'program': golden.program(),
            'draws': [1, 2, 4, 8],
            'repeats': 5,
            'warmup': 2,
        },
        {
            'id': 'exit-kernel',
            'kind': 'compute',
            'target': FLOAT,
            'compute': {
                'code': protocol.b64(known.KNOWN_KERNEL),
                'gprs': 1,
                'threads': [32, 1, 1],
                'groups': [8, 1, 1],
            },
            'draws': [1, 4],
            'repeats': 3,
            'readback': 'none',
        },
        {
            'id': 'output-kernel',
            'kind': 'compute',
            'target': FLOAT,
            'compute': OUTPUT_KERNEL.wire()
            | {'threads': [OUTPUT_THREADS, 1, 1], 'groups': [1, 1, 1], 'output': OUTPUT_THREADS * 8},
            'readback': 'full',
        },
    ]


def output_right(r: dict) -> bool:
    data = (r.get('output') or {}).get('data')
    return bool(data) and protocol.unb64(data) == struct.pack('<2I', *OUTPUT_WORDS) * OUTPUT_THREADS


def judge(results: dict[str, dict]) -> tuple[bool, list[str]]:
    lines, ok = [], True
    for e in calibration():
        informative = e['id'] in INFORMATIVE
        r = results.get(e['id'])
        if not r:
            lines.append(f'{e["id"]}: no result')
            ok = ok and informative
            continue
        gpu = [ns for run in r.get('runs', []) for ns in run.get('gpu_ns', [])]
        good = r['status'] == 'ok' and bool(gpu) and all(ns > 0 for ns in gpu)
        detail = f'{r["status"]} gpu_ns={sorted(gpu)[len(gpu) // 2] if gpu else "-"}'
        if e['kind'] == 'compute' and 'output' in e['compute']:
            good = good and output_right(r)
            detail += f' output={"every thread's known words" if output_right(r) else "wrong or missing"}'
        elif e.get('readback', 'summary') != 'none':
            distinct = (r.get('texels') or {}).get('distinct', [])
            right = len(distinct) == 1 and distinct[0]['value'] == KNOWN_TEXEL
            good = good and right
            detail += f' texels={"all the known answer" if right else distinct[:2]}'
        if r.get('why'):
            detail += f' why={r["why"]}'
        if r.get('kernel'):
            detail += f' kernel={"; ".join(r["kernel"])}'
        ok = ok and (good or informative)
        verdict = ('PASS' if good else 'FAIL') + (
            ' (informative: its encodings are new to the chip)' if informative else ''
        )
        lines.append(f'{e["id"]}: {verdict} {detail}')
    if dead := dead_bench(results.get('known')):
        lines.insert(0, dead)
    return ok, lines


def dead_bench(known_result: dict | None) -> str | None:
    """Give the one line that says the installed app cannot draw at all, when its own known program died."""
    if not known_result or known_result.get('status') != 'killed':
        return None
    cause = '; '.join(known_result.get('kernel', [])) or 'no GR line seen'
    return (
        f'BENCH DEAD: the app killed its own known program ({cause}). The installed app cannot draw: '
        'no known, fragment or readback experiment can run until it is rebuilt'
    )


def roots(udid: str | None) -> list[transport.Afc]:
    found = []
    for bundle in transport.BUNDLES:
        afc = transport.Afc(udid=udid, documents=bundle)
        if afc.ls('/Documents') is not None:
            found.append(afc)
    media = transport.Afc(udid=udid)
    if media.ls('/') is not None:
        found.append(media)
    return found


def prove(s: session.Session, into: Path) -> tuple[bool, list[str]]:
    outcome = s.run(protocol.batch(session.unique('calibration'), calibration()), into)
    ok, lines = judge(outcome.results)
    if not outcome.complete:
        lines.append(f'no done.json after {outcome.seconds:.0f} s (is the Bench screen open?)')
    return ok and outcome.complete, lines


def doctor(udid: str | None, into: Path) -> bool:
    udids = transport.devices()
    if not udids:
        print('no device: plug the Switch in while iOS runs, unlock it and trust this Mac')
        return False
    udid = udid or udids[0]
    print(f'device {udid}: iOS {session.paired(udid)}')
    candidates = roots(udid)
    if not candidates:
        print('neither house_arrest nor AFC answers: is HW Test installed, the device paired?')
        return False
    for afc in candidates:
        afc.mkdir(f'{afc.root}/inbox')
        afc.mkdir(f'{afc.root}/outbox')
    for afc in candidates:
        if 'status.json' not in (afc.ls(afc.root) or []):
            print(f'{afc.name}: the app is not watching it yet (open HW Test, tap Bench, or close and reopen Bench)')
            continue
        if (spoken := app_protocol(afc)) != protocol.PROTOCOL:
            print(f'{afc.name}: BENCH PROTOCOL MISMATCH: the app speaks {spoken}, this tool {protocol.PROTOCOL}')
            continue
        stream = loop.LogStream(udid)
        try:
            ok, lines = prove(session.Session(afc, stream), into)
        finally:
            stream.close()
        print('\n'.join('  ' + line for line in lines))
        if ok:
            session.remember(afc)
            print(f'loop proven through {afc.name}; later runs use it')
            return True
    return False


def proven_on_fake() -> tuple[bool, list[str]]:
    with tempfile.TemporaryDirectory() as scratch:
        s = session.fake(Path(scratch) / 'device')
        return prove(s, Path(scratch) / 'results')
