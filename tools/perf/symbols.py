"""Unslid symbol tables for the dyld shared cache, kernel and our own binaries.

Shared-cache addresses are unslid, so one table per IPSW build under
~/Library/Application Support/switch-ios/symbols/<build>/ names them without keeping a gigabyte shared cache on disk.
dsc.tsv holds the images' ranges and loads at once; dsc-functions.tsv.gz holds every code symbol (7.3 M for 24B119,
11.5 s and 2.5 GB to read whole), so the first lookup it is needed for writes a sorted index beside it
(dsc-functions.idx), which every later lookup maps and bisects.
Our own kernel and bundle offsets symbolize from unstripped build outputs (or .tsv tables extracted from them).
"""

from __future__ import annotations

import bisect
import gzip
import json
import mmap
import re
import shutil
import struct
import subprocess
import zlib
from array import array
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

SYMBOLS_ROOT = Path.home() / 'Library/Application Support/switch-ios/symbols'
MAX_SYMBOL_SPAN = 1 << 20
MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
DSC_MAGIC = b'dyld_v1'
NM_LINE = re.compile(r'^(?P<addr>[0-9a-fA-F]+)\s+[A-Za-z]\s+(?P<name>\S.*)$')
HEX_TOKEN = re.compile(r'^(?:0x)?[0-9a-fA-F]+$')
WHERE_DSC = re.compile(r'^dsc\s+0x(?P<addr>[0-9a-fA-F]+)$')
WHERE_MAIN = re.compile(r'^main\+0x(?P<off>[0-9a-fA-F]+)$')
WHERE_KERN = re.compile(r'^k\s+0x(?P<addr>[0-9a-fA-F]+)$')
FUNCTIONS = 'dsc-functions.tsv.gz'
FUNCTION_INDEX = 'dsc-functions.idx'
INDEX_HEAD = struct.Struct('<8sQQ')  # magic, symbols, images
INDEX_MAGIC = b'SWFN0003'
INDEX_BLOCK = 1024  # names per compressed block: a lookup inflates one


def build_function_index(table: Path, index: Path) -> None:
    """Write a function table's code symbols as a sorted index.

    After the head: the addresses (u64), each one's image (u16), the image names, each names block's offset (u64, one
    past the last), then the blocks, each INDEX_BLOCK names deflated.
    """
    rows: list[tuple[int, int, bytes]] = []
    images: dict[str, int] = {}
    with gzip.open(table, 'rt', errors='replace') as f:
        for line in f:
            if not line.startswith('S\t'):
                continue
            parts = line.rstrip('\n').split('\t')
            if len(parts) >= 4 and (addr := _parse_hex(parts[1])) is not None:
                img = images.setdefault(_base_name(parts[2]), len(images))
                rows.append((addr, img, parts[3].encode()))
    rows.sort(key=lambda r: r[0])
    blocks = [
        zlib.compress(b'\0'.join(r[2] for r in rows[k : k + INDEX_BLOCK]), 6) for k in range(0, len(rows), INDEX_BLOCK)
    ]
    offsets, at = array('Q'), 0
    for block in blocks:
        offsets.append(at)
        at += len(block)
    offsets.append(at)
    partial = index.with_suffix('.partial')
    with partial.open('wb') as f:
        f.write(INDEX_HEAD.pack(INDEX_MAGIC, len(rows), len(images)))
        array('Q', (r[0] for r in rows)).tofile(f)
        array('H', (r[1] for r in rows)).tofile(f)
        f.write(b''.join(name.encode() + b'\0' for name in images))
        offsets.tofile(f)
        for block in blocks:
            f.write(block)
    partial.replace(index)


class FunctionIndex:
    """A function index mapped read-only: the symbol at or before an address."""

    def __init__(self, index: Path):
        with index.open('rb') as f:
            self._map = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        magic, n, nimages = INDEX_HEAD.unpack_from(self._map, 0)
        if magic != INDEX_MAGIC:
            raise ValueError(f'{index} is not a function index of this version')
        view = memoryview(self._map)
        at = INDEX_HEAD.size
        self.addrs = view[at : at + 8 * n].cast('Q')
        self.image_of = view[at + 8 * n : at + 10 * n].cast('H')
        at += 10 * n
        self.images = []
        for _ in range(nimages):
            end = self._map.find(b'\0', at)
            self.images.append(self._map[at:end].decode('utf-8', 'replace'))
            at = end + 1
        nblocks = -(-n // INDEX_BLOCK)
        self.block_at = view[at : at + 8 * (nblocks + 1)].cast('Q')
        self.blocks = at + 8 * (nblocks + 1)
        self._names: dict[int, list[bytes]] = {}

    def _block(self, k: int) -> list[bytes]:
        if k not in self._names:
            if len(self._names) >= 64:
                self._names.clear()
            raw = self._map[self.blocks + self.block_at[k] : self.blocks + self.block_at[k + 1]]
            self._names[k] = zlib.decompress(raw).split(b'\0')
        return self._names[k]

    def lookup(self, addr: int) -> tuple[int, str] | None:
        i = bisect.bisect_right(self.addrs, addr) - 1
        if i < 0:
            return None
        name = self._block(i // INDEX_BLOCK)[i % INDEX_BLOCK].decode('utf-8', 'replace')
        return self.addrs[i], f'{self.images[self.image_of[i]]} {name}'


@dataclass(frozen=True)
class ImageRange:
    start: int
    end: int
    name: str


@dataclass(frozen=True)
class Symbol:
    addr: int
    name: str
    image: str = ''


@dataclass
class Table:
    addrs: list[int] = field(default_factory=list)
    syms: list[Symbol] = field(default_factory=list)
    image_starts: list[int] = field(default_factory=list)
    images: list[ImageRange] = field(default_factory=list)

    @property
    def empty(self) -> bool:
        return not self.addrs and not self.images

    def find_image(self, addr: int) -> ImageRange | None:
        i = bisect.bisect_right(self.image_starts, addr) - 1
        if i >= 0 and self.images[i].start <= addr < self.images[i].end:
            return self.images[i]
        return None

    def lookup(self, addr: int) -> str | None:
        img = self.find_image(addr)
        i = bisect.bisect_right(self.addrs, addr) - 1
        if i >= 0:
            sym = self.syms[i]
            in_bounds = addr - sym.addr < MAX_SYMBOL_SPAN and (img is None or sym.addr >= img.start)
            if in_bounds:
                off = addr - sym.addr
                label = sym.image or (img.name if img else '')
                suffix = f'+0x{off:x}' if off else ''
                return f'{label} {sym.name}{suffix}' if label else f'{sym.name}{suffix}'
        if img is not None:
            return f'{img.name}+0x{addr - img.start:x}'
        return None

    def region_names(self, region_mb: int) -> list[str]:
        lo = region_mb << 20
        hi = lo + (1 << 20)
        start = max(0, bisect.bisect_right(self.image_starts, lo) - 1)
        seen: list[str] = []
        for img in self.images[start:]:
            if img.start >= hi:
                break
            if img.end > lo and img.name not in seen:
                seen.append(img.name)
                if len(seen) == 3:
                    break
        return seen


def _base_name(path: str) -> str:
    return path.rsplit('/', 1)[-1]


def _parse_hex(raw: str) -> int | None:
    s = raw.strip()
    return int(s, 16) if HEX_TOKEN.match(s) else None


def build_table(syms: Iterable[Symbol], images: Iterable[ImageRange] = ()) -> Table:
    ordered_syms = sorted(syms, key=lambda s: s.addr)
    ordered_imgs = sorted(images, key=lambda im: im.start)
    return Table(
        addrs=[s.addr for s in ordered_syms],
        syms=ordered_syms,
        image_starts=[im.start for im in ordered_imgs],
        images=ordered_imgs,
    )


def parse_tsv(lines: Iterable[str]) -> Table:
    """Parse a symbol table: I/S TSV records, two-column hex-symbol TSV, or `nm -n` lines."""
    syms: list[Symbol] = []
    imgs: list[ImageRange] = []
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split('\t')
        if parts[0] == 'I' and len(parts) >= 4:
            lo, hi = _parse_hex(parts[1]), _parse_hex(parts[2])
            if lo is not None and hi is not None and hi > lo:
                imgs.append(ImageRange(lo, hi, _base_name(parts[3])))
        elif parts[0] == 'S' and len(parts) >= 3:
            addr = _parse_hex(parts[1])
            if addr is not None:
                img, name = (_base_name(parts[2]), parts[3]) if len(parts) >= 4 else ('', parts[2])
                syms.append(Symbol(addr, name, img))
        elif len(parts) == 2 and (addr := _parse_hex(parts[0])) is not None:
            syms.append(Symbol(addr, parts[1]))
        elif m := NM_LINE.match(line):
            syms.append(Symbol(int(m['addr'], 16), m['name'].strip()))
    return build_table(syms, imgs)


def _merge_tables(first: Table, second: Table) -> Table:
    if first.empty:
        return second
    if second.empty:
        return first
    return build_table([*first.syms, *second.syms], [*first.images, *second.images])


def is_dsc_binary(path: Path) -> bool:
    if not path.is_file():
        return False
    with path.open('rb') as f:
        return f.read(len(DSC_MAGIC)) == DSC_MAGIC


def is_macho64(path: Path) -> bool:
    if not path.is_file():
        return False
    with path.open('rb') as f:
        head = f.read(4)
    return len(head) == 4 and struct.unpack('<I', head)[0] == MH_MAGIC_64


def macho_text_base(path: Path) -> int:
    """Return a 64-bit Mach-O's __TEXT vmaddr so `nm` addresses become image-relative offsets."""
    with path.open('rb') as f:
        head = f.read(32)
        if len(head) < 32:
            return 0
        magic, _, _, _, ncmds, _, _, _ = struct.unpack('<IIIIIIII', head)
        if magic != MH_MAGIC_64:
            return 0
        for _ in range(ncmds):
            cmd_head = f.read(8)
            if len(cmd_head) < 8:
                break
            cmd, cmdsize = struct.unpack('<II', cmd_head)
            body = f.read(max(0, cmdsize - 8))
            if cmd == LC_SEGMENT_64 and len(body) >= 32:
                segname, vmaddr = struct.unpack('<16sQ', body[:24])
                if segname.split(b'\x00', 1)[0] == b'__TEXT':
                    return vmaddr
    return 0


def load_macho(path: Path, *, relative: bool) -> Table:
    """Read defined symbols from an unstripped Mach-O via `nm -n`."""
    if not shutil.which('nm') or not path.is_file():
        return Table()
    base = macho_text_base(path) if relative else 0
    done = subprocess.run(['nm', '-n', '--defined-only', str(path)], capture_output=True, text=True, check=False)
    if done.returncode != 0:
        return Table()
    syms = []
    for line in done.stdout.splitlines():
        if m := NM_LINE.match(line.strip()):
            addr = int(m['addr'], 16)
            if addr >= base:
                syms.append(Symbol(addr - base, m['name'].strip()))
    return build_table(syms)


def _parse_int_field(val: object) -> int:
    if isinstance(val, int):
        return val
    if isinstance(val, str):
        return _parse_hex(val) or 0
    return 0


def _parse_ipsw_entry(item: object) -> tuple[int, int, str] | None:
    if not isinstance(item, dict):
        return None
    addr = _parse_int_field(
        item.get('load_addr') or item.get('load_address') or item.get('address') or item.get('vmaddr') or 0
    )
    size = _parse_int_field(item.get('size') or 0)
    name = item.get('name') or item.get('path') or ''
    return (addr, size, _base_name(str(name))) if addr and name else None


def _ipsw_images(dsc_path: Path) -> list[ImageRange]:
    if not shutil.which('ipsw'):
        return []
    done = subprocess.run(
        ['ipsw', 'dyld', 'info', '--dylibs', '--json', str(dsc_path)], capture_output=True, text=True, check=False
    )
    if done.returncode != 0:
        return []
    try:
        payload = json.loads(done.stdout)
    except json.JSONDecodeError:
        return []
    raw_list = payload.get('dylibs', payload) if isinstance(payload, dict) else payload
    if not isinstance(raw_list, list):
        return []
    parsed = sorted(e for item in raw_list if (e := _parse_ipsw_entry(item)) is not None)
    out: list[ImageRange] = []
    for i, (addr, size, name) in enumerate(parsed):
        end = addr + size if size > 0 else (parsed[i + 1][0] if i + 1 < len(parsed) else addr + MAX_SYMBOL_SPAN)
        if end > addr:
            out.append(ImageRange(addr, end, name))
    return out


def _ipsw_a2s(dsc_path: str, addr: int) -> str | None:
    if not shutil.which('ipsw'):
        return None
    done = subprocess.run(
        ['ipsw', 'dyld', 'a2s', '--image', dsc_path, f'0x{addr:x}'], capture_output=True, text=True, check=False
    )
    if done.returncode != 0:
        return None
    for line in done.stdout.splitlines():
        text = line.strip()
        if text and not text.startswith('level='):
            return text
    return None


@dataclass
class Symbols:
    """Symbol tables for a session: shared cache, kernel and our own binaries, with offset fallback."""

    dsc: Table = field(default_factory=Table)
    kernel: Table = field(default_factory=Table)
    binaries: dict[str, Table] = field(default_factory=dict)
    dsc_cache: str | None = None
    dsc_functions: Path | None = None  # dsc-functions.tsv.gz, indexed at its first use
    _functions: FunctionIndex | None = field(default=None, repr=False)
    _live: dict[int, str | None] = field(default_factory=dict, repr=False)

    @property
    def active(self) -> bool:
        return (
            not self.dsc.empty
            or not self.kernel.empty
            or bool(self.binaries)
            or bool(self.dsc_cache)
            or bool(self.dsc_functions)
        )

    def function(self, addr: int) -> str | None:
        """Return the shared-cache function at an unslid address from dsc-functions, inside the image that holds it."""
        if self.dsc_functions is None:
            return None
        if self._functions is None:
            index = self.dsc_functions.with_name(FUNCTION_INDEX)
            if not index.is_file() or index.stat().st_mtime < self.dsc_functions.stat().st_mtime:
                build_function_index(self.dsc_functions, index)
            self._functions = FunctionIndex(index)
        found = self._functions.lookup(addr)
        img = self.dsc.find_image(addr)
        if not found or addr - found[0] >= MAX_SYMBOL_SPAN or (img is not None and found[0] < img.start):
            return None
        off = addr - found[0]
        return found[1] + (f'+0x{off:x}' if off else '')

    def dsc_addr(self, addr: int) -> str:
        if self.dsc_functions is not None and (name := self.function(addr)):
            return name
        if hit := self.dsc.lookup(addr):
            return hit
        if self.dsc_cache:
            if addr not in self._live:
                self._live[addr] = _ipsw_a2s(self.dsc_cache, addr)
            if self._live[addr]:
                return self._live[addr]  # type: ignore[return-value]
        return f'dsc 0x{addr:x}'

    def dsc_region(self, region_mb: int) -> str:
        names = self.dsc.region_names(region_mb)
        return f'{"/".join(names)} (dsc {region_mb:x})' if names else f'dsc {region_mb:x}'

    def kernel_addr(self, addr: int) -> str:
        return self.kernel.lookup(addr) or f'k 0x{addr:x}'

    def image_addr(self, name: str, offset: int, uuid: str = '') -> str:
        base = _base_name(name)
        table = self.binaries.get(uuid.upper()) or self.binaries.get(base)
        if table is not None and (hit := table.lookup(offset)):
            return hit if hit.startswith(f'{base} ') else f'{base} {hit}'
        return f'{base}+0x{offset:x}'

    def resolve_where(self, text: str, proc: str = '') -> str:
        s = text.strip()
        if m := WHERE_DSC.match(s):
            return self.dsc_addr(int(m['addr'], 16))
        if proc and (m := WHERE_MAIN.match(s)):
            return self.image_addr(proc, int(m['off'], 16))
        if m := WHERE_KERN.match(s):
            return self.kernel_addr(int(m['addr'], 16))
        return s


def _load_dir(root: Path, syms: Symbols) -> None:
    if not root.is_dir():
        return
    for path in sorted(root.iterdir()):
        if not path.is_file():
            continue
        if path.name == FUNCTIONS:
            syms.dsc_functions = path
        elif path.name == 'dsc.tsv':
            with path.open(errors='replace') as f:
                syms.dsc = _merge_tables(syms.dsc, parse_tsv(f))
        elif path.name == 'kernel.tsv':
            with path.open(errors='replace') as f:
                syms.kernel = _merge_tables(syms.kernel, parse_tsv(f))
        elif path.suffix == '.tsv':
            with path.open(errors='replace') as f:
                syms.binaries[path.stem] = parse_tsv(f)
        elif is_macho64(path):
            if path.stem.startswith('kernel'):
                syms.kernel = _merge_tables(syms.kernel, load_macho(path, relative=False))
            else:
                syms.binaries[path.name] = load_macho(path, relative=True)


def _load_dsc_arg(dsc: Path, syms: Symbols) -> None:
    if dsc.is_dir():
        _load_dir(dsc, syms)
    elif is_dsc_binary(dsc):
        syms.dsc_cache = str(dsc)
        imgs = _ipsw_images(dsc)
        if imgs:
            syms.dsc = _merge_tables(syms.dsc, build_table((), imgs))
    elif dsc.is_file():
        with dsc.open(errors='replace') as f:
            syms.dsc = _merge_tables(syms.dsc, parse_tsv(f))


def load(
    dsc: str | None = None,
    kernel: str | None = None,
    symbols_dir: str | None = None,
    build: str = '',
) -> Symbols:
    """Load symbol tables from explicit paths or ~/Library/Application Support/switch-ios/symbols/<build>/."""
    syms = Symbols()
    auto = Path(symbols_dir) if symbols_dir else (SYMBOLS_ROOT / build if build else None)
    if auto is not None:
        _load_dir(auto, syms)
    if dsc:
        _load_dsc_arg(Path(dsc), syms)
    if kernel:
        kp = Path(kernel)
        if is_macho64(kp):
            syms.kernel = _merge_tables(syms.kernel, load_macho(kp, relative=False))
        elif kp.is_file():
            with kp.open(errors='replace') as f:
                syms.kernel = _merge_tables(syms.kernel, parse_tsv(f))
    return syms


def write_tsv(out: Path, table: Table) -> int:
    """Write a Table as sorted I/S TSV lines and return the row count."""
    out.parent.mkdir(parents=True, exist_ok=True)
    rows = [f'I\t{im.start:x}\t{im.end:x}\t{im.name}' for im in table.images]
    rows += [f'S\t{s.addr:x}\t{s.image}\t{s.name}' if s.image else f'S\t{s.addr:x}\t{s.name}' for s in table.syms]
    out.write_text('\n'.join(rows) + ('\n' if rows else ''))
    return len(rows)


def ipsw_symbol(line: str) -> str | None:
    """Return `ipsw dyld symaddr`'s line (address, section, name, image, tab-separated) as an S row of code only."""
    parts = line.rstrip('\n').split('\t')
    if len(parts) < 4 or '__TEXT.__text' not in parts[1] or not parts[0].startswith('0x'):
        return None
    return f'S\t{parts[0][2:].rstrip(":")}\t{_base_name(parts[3])}\t{parts[2]}\n'


def extract_dsc(dsc_path: Path, out_tsv: Path) -> int:
    """Extract a dyld shared cache's unslid image ranges into dsc.tsv and its code symbols into dsc-functions.tsv.gz."""
    imgs = _ipsw_images(dsc_path)
    if not imgs:
        raise SystemExit(f'could not read shared cache images from {dsc_path} (is `ipsw` installed?)')
    rows = write_tsv(out_tsv, build_table((), imgs))
    with (
        subprocess.Popen(['ipsw', 'dyld', 'symaddr', str(dsc_path)], stdout=subprocess.PIPE, text=True) as proc,
        gzip.open(out_tsv.with_name(FUNCTIONS), 'wt') as out,
    ):
        for line in proc.stdout or ():
            if row := ipsw_symbol(line):
                out.write(row)
                rows += 1
    return rows


def extract_macho(macho_path: Path, out_tsv: Path, *, relative: bool) -> int:
    """Extract symbols from an unstripped Mach-O via `nm -n` into a reusable TSV."""
    table = load_macho(macho_path, relative=relative)
    if table.empty:
        raise SystemExit(f'no symbols found in {macho_path}')
    return write_tsv(out_tsv, table)
