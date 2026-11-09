"""The lock-screen poster as the system last drew it: PosterBoard's snapshot bundles on the card's Data volume.

Each bundle holds a poster's levels as ATX files, in the panel's own orientation (720x1280): Composite (all levels),
Floating (the level drawn above the time) and the rest. A landscape bundle (AssetSize wider than tall) whose
Composite matches the portrait one pixel for pixel means the poster drew its portrait composition for a landscape
screen (boot 15: correlation 1.000, Celosia's petals down the left edge, over the time). The time band's coverage by
the Floating level is what the user sees of the time.
"""

import plistlib
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

STORE = 'Library/Application Support/PRBPosterExtensionDataStore'
CONTAINERS = 'mobile/Containers/Data/Application'
BUNDLES = 'scratch/SnapshotCache.cachedb/PosterSnapshotCache.bundle/SnapshotBundles'
INFO_KEY = 'PUIPosterSnapshotBundleInfoKey'
ATX_MAGIC = b'AAPL\r\n\x1a\n'
KTX_MAGIC = b'\xabKTX 11\xbb\r\n\x1a\n'
GL_RGBA = 0x1908
# The time sits at the top centre of the screen, in the orientation the user holds it: (left, top, right, bottom).
TIME_BAND = (0.25, 0.04, 0.75, 0.34)
SAME_COMPOSITION = 0.95
GRID = (36, 64)


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    rgba: bytes

    def pixel(self, x: int, y: int) -> tuple[int, int, int, int]:
        at = (y * self.width + x) * 4
        return tuple(self.rgba[at : at + 4])


@dataclass(frozen=True)
class Snapshot:
    """A poster render PosterBoard kept: a snapshot bundle's levels, or a runtime snapshot (one composite image)."""

    provider: str
    info: dict
    composite: Path
    floating: Path | None = None

    def value(self, key: str) -> object:
        return self.info.get(INFO_KEY + key)

    def landscape(self) -> bool:
        width, height = asset_size(self.value('AssetSize'))
        return width > height


def asset_size(text: str | None) -> tuple[float, float]:
    width, _, height = (text or '{0, 0}').strip('{}').partition(',')
    return float(width), float(height)


def atx_payload(data: bytes) -> tuple[int, int, int, bytes]:
    """Return an ATX file's GL internal format, width, height and compressed payload."""
    if not data.startswith(ATX_MAGIC):
        raise ValueError('not an ATX file')
    at, head, payload = len(ATX_MAGIC), None, None
    while at + 8 <= len(data):
        size, tag = struct.unpack_from('<I4s', data, at)
        chunk = data[at + 8 : at + 8 + size]
        if tag == b'HEAD':
            head = struct.unpack_from('<8I', chunk)
        elif tag == b'astc':
            payload = chunk[4:]
        at += 8 + size
    if head is None or payload is None:
        raise ValueError('ATX file without HEAD or astc chunks')
    return head[4], head[6], head[7], payload


def ktx(internal_format: int, width: int, height: int, payload: bytes) -> bytes:
    """Wrap the payload as a KTX 1 file, which ImageIO decodes (its own ATX reader returns a flat colour)."""
    header = struct.pack('<13I', 0x04030201, 0, 1, 0, internal_format, GL_RGBA, width, height, 0, 0, 1, 1, 0)
    return KTX_MAGIC + header + struct.pack('<I', len(payload)) + payload


def bmp_rgba(data: bytes) -> Image:
    """Read sips' 32-bit BITFIELDS BMP (BGRA, rows top-down when the height is negative) as RGBA."""
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<ii', data, 18)
    if struct.unpack_from('<H', data, 28)[0] != 32:
        raise ValueError('not a 32-bit BMP')
    rows = [data[offset + r * width * 4 : offset + (r + 1) * width * 4] for r in range(abs(height))]
    if height > 0:
        rows.reverse()
    bgra = b''.join(rows)
    rgba = bytearray(len(bgra))
    rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4] = bgra[2::4], bgra[1::4], bgra[0::4], bgra[3::4]
    return Image(width, abs(height), bytes(rgba))


def decode(path: Path, png: Path | None = None) -> Image:
    """Decode an ATX file through ImageIO (sips); `png` also keeps the pixels as a PNG."""
    with tempfile.TemporaryDirectory(prefix='switch-atx-') as scratch:
        source = Path(scratch) / 'level.ktx'
        source.write_bytes(ktx(*atx_payload(path.read_bytes())))
        out = Path(scratch) / 'level.bmp'
        subprocess.run(['sips', '-s', 'format', 'bmp', str(source), '--out', str(out)], capture_output=True, check=True)
        if png:
            convert = ['sips', '-s', 'format', 'png', str(source), '--out', str(png)]
            subprocess.run(convert, capture_output=True, check=True)
        return bmp_rgba(out.read_bytes())


def luma_grid(image: Image, cols: int = GRID[0], rows: int = GRID[1]) -> list[float]:
    grid = []
    for row in range(rows):
        for col in range(cols):
            r, g, b, _ = image.pixel(col * image.width // cols, row * image.height // rows)
            grid.append(0.3 * r + 0.59 * g + 0.11 * b)
    return grid


def correlation(a: list[float], b: list[float]) -> float:
    def normalized(values: list[float]) -> list[float]:
        mean = sum(values) / len(values)
        spread = (sum((v - mean) ** 2 for v in values) / len(values)) ** 0.5 or 1.0
        return [(v - mean) / spread for v in values]

    return sum(x * y for x, y in zip(normalized(a), normalized(b), strict=True)) / len(a)


def band_coverage(floating: Image, orientation: int, band: tuple[float, ...] = TIME_BAND, step: int = 4) -> float:
    """Return the share of the time band the Floating level covers (alpha at least half)."""
    left, top, right, bottom = band
    covered = total = 0
    for y in range(0, floating.height, step):
        for x in range(0, floating.width, step):
            u, v = screen_point(x / floating.width, y / floating.height, orientation)
            if left <= u < right and top <= v < bottom:
                total += 1
                covered += floating.pixel(x, y)[3] >= 128
    return covered / total if total else 0.0


def screen_point(px: float, py: float, orientation: int) -> tuple[float, float]:
    """Map a panel point (fractions) onto the screen as held, by UIInterfaceOrientation (4: panel's left edge up)."""
    if orientation == 4:
        return 1 - py, px
    if orientation == 3:
        return py, 1 - px
    if orientation == 2:
        return 1 - px, 1 - py
    return px, py


def snapshots(data: Path) -> list[Snapshot]:
    """Every snapshot bundle and runtime snapshot of every poster configuration in PosterBoard's data store."""
    found = []
    for store in sorted((data / CONTAINERS).glob(f'*/{STORE}/*/Extensions')):
        for version in sorted(store.glob('*/configurations/*/versions/*')):
            provider = version.relative_to(store).parts[0]
            for info in sorted(version.glob(f'{BUNDLES}/*/Snapshot.pks/Contents/Info.plist')):
                resources = info.parent.parent / 'Resources'
                floating = resources / 'Floating.atx'
                level = floating if floating.is_file() else None
                found.append(Snapshot(provider, read_plist(info), resources / 'Composite.atx', level))
            for info in sorted(version.glob('RuntimeSnapshotMetadata-*.plist')):
                image = version / (info.stem.replace('RuntimeSnapshotMetadata-', 'RuntimeSnapshot-', 1) + '.atx')
                found.append(Snapshot(provider, read_plist(info), image))
    return [s for s in found if s.info and s.composite.is_file()]


def read_plist(path: Path) -> dict:
    try:
        return plistlib.loads(path.read_bytes())
    except (OSError, plistlib.InvalidFileException, ValueError):
        return {}


def drawn(grid: list[float]) -> bool:
    """Tell a render with content: boot 15 kept single-colour snapshots of posters that timed out."""
    return max(grid) - min(grid) >= 4


def newest_drawn(candidates: list[Snapshot], grids: dict) -> Snapshot | None:
    """Return the newest snapshot of `candidates` with content, decoding only as far back as it takes."""
    for snapshot in sorted(candidates, key=lambda s: s.value('DateCreated') or '', reverse=True):
        if snapshot.composite not in grids:
            grids[snapshot.composite] = luma_grid(decode(snapshot.composite))
        if drawn(grids[snapshot.composite]):
            return snapshot
    return None


def lock_poster(data: Path, out: Path | None = None) -> dict | None:
    """Compare the poster's newest landscape render with its newest portrait one; measure its Floating level's cover.

    Each `out`/lock-poster/*.png is a level it read, in the panel's orientation.
    """
    found = snapshots(data)
    grids: dict = {}
    landscape = newest_drawn([s for s in found if s.landscape()], grids)
    if not landscape:
        return None
    mine = [s for s in found if s.provider == landscape.provider and not s.landscape()]
    style = landscape.value('UserInterfaceStyle')
    portrait = newest_drawn([s for s in mine if s.value('UserInterfaceStyle') == style], grids) or newest_drawn(
        mine, grids
    )
    keep = out / 'lock-poster' if out else None
    if keep:
        keep.mkdir(parents=True, exist_ok=True)
        decode(landscape.composite, keep / 'landscape-composite.png')
    result = {
        'provider': landscape.provider,
        'created': landscape.value('DateCreated'),
        'device_orientation': int(landscape.value('DeviceInterfaceOrientation') or 1),
        'interface_orientation': landscape.value('InterfaceOrientation'),
    }
    if portrait:
        if keep:
            decode(portrait.composite, keep / 'portrait-composite.png')
        result['portrait_correlation'] = round(correlation(grids[landscape.composite], grids[portrait.composite]), 3)
    layered = newest_drawn(
        [s for s in found if s.landscape() and s.floating and s.provider == landscape.provider], grids
    )
    if layered:
        floating = decode(layered.floating, keep and keep / 'landscape-floating.png')
        orientation = int(layered.value('DeviceInterfaceOrientation') or 1)
        result['time_band_covered'] = round(band_coverage(floating, orientation), 3)
    return result


def observe(data: Path, out: Path | None = None) -> dict | None:
    """Run lock_poster; None when the card holds no readable snapshot (or sips cannot decode one)."""
    try:
        return lock_poster(data, out)
    except (OSError, ValueError, subprocess.CalledProcessError):
        return None


def summary_line(poster: dict | None) -> str | None:
    """Give the HW Test summary line: whether the landscape poster was composed for landscape, and the time's cover."""
    if not poster:
        return None
    parts = [f'lock poster {poster["provider"]} in landscape (orientation {poster["device_orientation"]})']
    corr = poster.get('portrait_correlation')
    if corr is not None:
        reused = 'REUSES the portrait composition' if corr >= SAME_COMPOSITION else 'composed for landscape'
        parts.append(f'{reused} (correlation {corr:.2f})')
    covered = poster.get('time_band_covered')
    if covered is not None:
        parts.append(f'floating level over {covered:.0%} of the time band')
    return '; '.join(parts)
