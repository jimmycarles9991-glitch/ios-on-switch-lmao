"""One session on the device, one report: pull the log archive and the reports over USB, then attribute what was slow.

`pull` copies the running Switch's log archive (`idevicesyslog archive`) and its crash reports (`idevicecrashreport`)
into a new folder and writes the report there; `report` reads an archive (or ndjson files) already on disk; `symbols`
extracts reusable unslid symbol tables under ~/Library/Application Support/switch-ios/symbols/<build>/.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tarfile
from datetime import UTC, datetime
from pathlib import Path

from tools.perf import dumps, ips, report, session, source, symbols

KCDATA = Path(__file__).resolve().parents[2] / 'kernel/xnu-12377.121.6/tools/lldbmacros/kcdata.py'


def pull_commands(out: Path, udid: str | None, age_s: int) -> list[list[str]]:
    """Return the commands that pull one session into out: the log archive as a tar, then the crash folder."""
    device = ['-u', udid] if udid else []
    return [
        ['idevicesyslog', *device, 'archive', str(out / 'archive.tar'), '--age-limit', str(age_s)],
        ['idevicecrashreport', *device, '-k', '-e', str(out / 'crash')],
    ]


def pull(out: Path, udid: str | None, age_s: int) -> Path:
    """Pull a session into a new folder under out and return its .logarchive."""
    folder = out / datetime.now(UTC).strftime('%Y%m%d-%H%M%S')
    (folder / 'crash').mkdir(parents=True)
    for cmd in pull_commands(folder, udid, age_s):
        if not shutil.which(cmd[0]):
            raise SystemExit(f'{cmd[0]} is not installed (brew install libimobiledevice)')
        subprocess.run(cmd, check=True)
    staging = folder / 'archive'
    staging.mkdir()
    with tarfile.open(folder / 'archive.tar') as tar:
        tar.extractall(staging, filter='data')
    (folder / 'archive.tar').unlink()
    archive = folder / 'boot.logarchive'
    archive_root(staging).rename(archive)
    if staging.exists():
        staging.rmdir()
    return archive


def archive_root(folder: Path) -> Path:
    """Return the log archive in an extracted pull: the folder itself, or the one folder in it, holding Info.plist."""
    if (folder / 'Info.plist').exists():
        return folder
    inside = [d for d in folder.iterdir() if d.is_dir() and (d / 'Info.plist').exists()]
    if len(inside) != 1:
        raise SystemExit(f'no log archive in {folder}: no Info.plist at its top or one level down')
    return inside[0]


def _detect_build(reports: list[ips.Report], boots: set[str]) -> str:
    for r in reports:
        if r.build and (not boots or r.boot in boots):
            return r.build
    return next((r.build for r in reports if r.build), '')


def write_reports(paths: list[str], crashes: str | None, args: argparse.Namespace) -> list[str]:
    recs, images = source.read(paths, args.start, args.end)
    kcdata = args.kcdata or (str(KCDATA) if KCDATA.is_file() else None)
    scanned = ips.scan(crashes)
    boots = {b.upper() for b in source.by_boot(recs) if b}
    build = args.build or _detect_build(scanned, boots)
    syms = symbols.load(args.dsc, args.kernel, args.symbols, build)
    pulled = report.Pulled(scanned, images, ips.spin_files(crashes), dumps.find(crashes), kcdata, syms)
    texts = []
    for boot, boot_recs in source.by_boot(recs).items():
        if not boot or (args.boot and not boot.upper().startswith(args.boot.upper())):
            continue
        texts.append(report.render(session.build(boot, boot_recs), pulled))
    return texts


def run_symbols(args: argparse.Namespace) -> int:
    out_dir = Path(args.out) if args.out else symbols.SYMBOLS_ROOT / args.build
    written = 0
    if args.dsc:
        n = symbols.extract_dsc(Path(args.dsc), out_dir / 'dsc.tsv')
        print(f'wrote {n} shared-cache image ranges to {out_dir / "dsc.tsv"}', file=sys.stderr)
        written += n
    if args.kernel:
        n = symbols.extract_macho(Path(args.kernel), out_dir / 'kernel.tsv', relative=False)
        print(f'wrote {n} kernel symbols to {out_dir / "kernel.tsv"}', file=sys.stderr)
        written += n
    for bin_path in args.binary or []:
        bp = Path(bin_path)
        n = symbols.extract_macho(bp, out_dir / f'{bp.name}.tsv', relative=True)
        print(f'wrote {n} symbols to {out_dir / (bp.name + ".tsv")}', file=sys.stderr)
        written += n
    if written == 0:
        raise SystemExit('pass at least one of --dsc, --kernel, or --binary')
    return 0


def _add_report_flags(p: argparse.ArgumentParser) -> None:
    p.add_argument('--out', help='write the report here instead of printing it')
    p.add_argument('--boot', help='only the boot whose UUID starts with this')
    p.add_argument('--start', help="log show's --start, e.g. '2026-11-23 00:15:00'")
    p.add_argument('--end', help="log show's --end")
    p.add_argument('--dsc', help='unslid shared-cache .tsv table, symbol directory, or dyld_shared_cache file')
    p.add_argument('--kernel', help='unstripped kernel Mach-O or kernel.tsv for kernel addresses')
    p.add_argument('--symbols', help='symbol dir (default: ~/Library/Application Support/switch-ios/symbols/<build>)')
    p.add_argument('--build', default='', help='IPSW build id (e.g. 24B119) for symbol directory lookup')


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--kcdata', help="XNU's tools/lldbmacros/kcdata.py, which reads a dump's stackshot")
    sub = parser.add_subparsers(dest='command', required=True)
    rep = sub.add_parser('report', help='report on archives already pulled')
    rep.add_argument('logs', nargs='+', help='.logarchive folders (absolute under bazel run) or ndjson files')
    rep.add_argument('--crashes', help='the pulled crash folder (idevicecrashreport -e)')
    pul = sub.add_parser('pull', help='pull a session from the device over USB, then report on it')
    pul.add_argument('folder', help='where the session folder is made (absolute under bazel run)')
    pul.add_argument('--udid', help='the device, when more than one is plugged in')
    pul.add_argument('--age', type=int, default=3600, help='seconds of log to pull (default 3600)')
    for p in (rep, pul):
        _add_report_flags(p)
    sym = sub.add_parser('symbols', help='extract unslid symbol tables for one build')
    sym.add_argument('--build', required=True, help='IPSW build id (e.g. 24B119 or 21H461)')
    sym.add_argument('--dsc', help='dyld_shared_cache file to extract image ranges from via ipsw dyld')
    sym.add_argument('--kernel', help='unstripped kernel Mach-O to extract kernel.tsv from via nm')
    sym.add_argument('--binary', action='append', help='unstripped app/bundle Mach-O (repeatable)')
    sym.add_argument('--out', help='output dir (default: ~/Library/Application Support/switch-ios/symbols/<build>)')
    args = parser.parse_args(argv)
    if args.command == 'symbols':
        return run_symbols(args)
    if args.command == 'pull':
        archive = pull(Path(args.folder), args.udid, args.age)
        logs, crashes = [str(archive)], str(archive.parent / 'crash')
        args.out = args.out or str(archive.parent / 'report.txt')
    else:
        logs, crashes = args.logs, args.crashes
    text = '\n'.join(write_reports(logs, crashes, args))
    if args.out:
        Path(args.out).write_text(text)
        print(f'wrote {args.out}', file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
