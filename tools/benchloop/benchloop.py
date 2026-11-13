"""bazel run //tools/benchloop -- <command>: experiments on the Switch's GPU while HW Test's Bench screen is open.

    doctor                     find the device and a folder both sides reach; prove the loop with the calibration batch
    run <batch.json>           one batch as written (protocol.py's format, `meta` and `expect` kept on the host)
    latency [--family F ...] [--op OP ...]   SM53 latency, throughput, dual issue and reuse; fitted table for sm50.rs
    blend <table.json> ...     an iterated-blend microcode table against its formula (blend.py documents the table)
    rop                        the ROP's float to unorm rounding, against each rounding model
    precision                  the store and fixed-function blend under every ROP option set the other way (sweeps.py)
    iterated [microcode.tsv]   the iterated blend unit with colour 0 bound alone; the table's programs after the setups
    twod                       the 2D engine's fills, scaled copies and resolves, with their 3D and copy-engine compares
    store16                    the ROP's float to unorm16 rounding (R16, RGBA16)
    compressed                 draws into compressible memory through the copy engine, against a plain target
    cstall                     the stall family as kernels storing their results (latency by correctness)

--fake <folder> runs on the fake device (fakedevice.py) instead of USB. Results land in --out (default
~/Library/Application Support/switch-ios/benchloop/<time>/): the device's files, results.tsv and report.json.
"""

import argparse
import json
import sys
import time
from pathlib import Path

from tools.benchloop import blend, doctor, floatadd, report, rop, session, sweeps
from tools.benchloop.generators import GENERATORS


def parse(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('command', choices=('doctor', 'run', 'latency', 'blend', 'rop', 'floatadd', *GENERATORS))
    p.add_argument('inputs', nargs='*', type=Path)
    p.add_argument('--udid')
    p.add_argument('--fake', type=Path, help='a folder the fake device works in')
    p.add_argument('--out', type=Path)
    p.add_argument('--family', action='append', default=[])
    p.add_argument('--op', action='append', default=[])
    p.add_argument('--format', default='bgra8unorm')
    p.add_argument('--blend-enable', choices=('as-is', 'both'), default='as-is')
    return p.parse_args(argv)


def connect(args: argparse.Namespace) -> session.Session:
    return session.fake(args.fake) if args.fake else session.usb(args.udid)


def experiments(args: argparse.Namespace) -> list[dict]:
    if args.command == 'run':
        return [e for path in args.inputs for e in json.loads(path.read_text())['experiments']]
    if args.command == 'latency':
        from tools.benchloop.sm53 import latency  # noqa: PLC0415  the generator pulls in the interpreter

        return latency.generate(*([args.family] if args.family else []), ops=args.op or None)
    if args.command == 'blend':
        enables = (None,) if args.blend_enable == 'as-is' else (True, False)
        return [e for path in args.inputs for e in blend.generate(blend.Table.load(path), args.format, 32, enables)]
    if args.command == 'floatadd':
        return floatadd.generate()
    if args.command == 'iterated':
        return sweeps.iterated(args.inputs[0] if args.inputs else None)
    return GENERATORS[args.command]() if args.command in GENERATORS else rop.generate()


def main(argv: list[str]) -> int:
    args = parse(argv)
    out = args.out or session.STATE / time.strftime('%Y%m%d-%H%M%S')
    out.mkdir(parents=True, exist_ok=True)
    if args.command == 'doctor':
        if args.fake:
            ok, lines = doctor.prove(session.fake(args.fake), out)
            print('\n'.join(lines))
            return 0 if ok else 1
        return 0 if doctor.doctor(args.udid, out) else 1
    planned = experiments(args)
    s = connect(args)
    try:
        results = s.run_all(planned, args.command, out)
    finally:
        s.close()
    summary = report.write(args.command, planned, results, out, [Path(p) for p in args.inputs])
    print(summary)
    print(f'results in {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
