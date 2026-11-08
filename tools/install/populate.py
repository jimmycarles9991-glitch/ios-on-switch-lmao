"""What goes into the composed container's Data and Preboot volumes, mounted on the Mac.

Who owns each file goes to an owner sink (owners.py): a Journal given offline after a run as the user, or Immediate
for a run as root; None leaves owners alone (an unprivileged test, not bootable).

Data     the System volume's /private/var (once), mobile's Preferences, the boot package's overlay, the Metal bundle
         (its stager, as //deploy:sd --stage-gpu) and the shader seed
Preboot  the two cryptex images, checked against the IPSW pins
"""

import os
import shutil
import sys
from pathlib import Path

from tools.common import files, hashing
from tools.install import host, owners as ownership, progress

SKIP = ('.fseventsd', '.Spotlight-V100', '.Trashes')


def seed_data(system: Path, data: Path, owners: ownership.Owners | None, log: host.Log, reseed: bool = False) -> None:
    if (data / 'mobile').is_dir() and not reseed:
        log('data: already seeded (mobile/ exists); kept (--reseed-data replaces it)')
        return
    entries = [p for p in data.iterdir() if p.name not in SKIP]
    if entries:
        if not reseed:
            raise host.HostError('Data is not empty but has no mobile/: refusing to merge (use --reseed-data)')
        log('data: --reseed-data: clearing', len(entries), 'entries')
        for p in entries:
            if p.is_dir() and not p.is_symlink():
                shutil.rmtree(p)
            else:
                p.unlink()
    start = shutil.disk_usage(data).used
    with progress.step(
        'data: seeding from System /private/var',
        allocated(system / 'private/var'),
        probe=lambda: shutil.disk_usage(data).used - start,
    ):
        host.run(
            'ditto',
            '--rsrc',
            '--extattr',
            '--acl',
            '--preserveHFSCompression',
            '--nocache',
            str(system / 'private/var'),
            str(data),
            log=log,
        )
    prefs = data / 'mobile/Library/Preferences'
    prefs.mkdir(parents=True, exist_ok=True)
    data.chmod(0o755)
    if owners:
        owners.set(data, 0, 0, 0o755)
        for p in (data / 'mobile/Library', prefs):
            owners.set(p, 501, 501)
    log(
        'data: seeded from System /private/var ({} entries){}'.format(
            sum(1 for _ in data.rglob('*')), '' if owners else ' WITHOUT owners (unprivileged test: not bootable)'
        )
    )


def allocated(root: Path) -> int:
    """Return the bytes the files below `root` take on disk (compressed files count as stored)."""
    return sum(
        os.lstat(os.path.join(directory, name)).st_blocks * 512
        for directory, _, names in os.walk(root)
        for name in names
    )


def cryptex_images(
    spec: dict, pins: dict, sources: dict[str, str | Path], expected: dict[str, str] | None = None
) -> dict[str, tuple[Path, str]]:
    """Return {member: (image, sha256)} for the cryptexes the spec lists.

    `sources`: {"system_cryptex": path, "app_cryptex": path}; `expected`: {member: sha256} for rebuilt images, which
    the IPSW pins do not describe.
    """
    expected = expected or {}
    images = {}
    for item in spec['cryptexes']:
        member = item['member']
        images[member] = (Path(sources[member]), expected.get(member) or pins['members'][member]['content_sha256'])
    return images


def stage_cryptexes(
    preboot: Path, spec: dict, images: dict[str, tuple[Path, str]], owners: ownership.Owners | None, log: host.Log
) -> None:
    """Stage the cryptex images (cryptex_images) into Preboot, each checked against its sha256."""
    for item in spec['cryptexes']:
        member = item['member']
        source, want = images[member]
        relative = item['path'][len('/private/preboot/') :]
        if Path(relative).name != source.name:
            raise host.HostError('{}: the kernel opens {}, the image is {}'.format(member, item['path'], source))
        target = preboot / relative
        if target.is_file() and target.stat().st_size == source.stat().st_size and hashing.sha256_file(target) == want:
            log(f'preboot: {relative} unchanged')
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        with (
            files.replacing(target) as partial,
            progress.step(f'preboot: staging {source.name}', 2 * source.stat().st_size),
        ):
            host.copy_file_parallel(source, partial)
            if hashing.sha256_file(partial, progress.advance) != want:
                raise host.HostError(f'{relative}: copy does not match its sha256')
        target.chmod(0o644)
        if owners:
            owners.set(preboot, 0, 0, 0o755)
            owners.set(target.parent, 0, 0)
            owners.set(target, 0, 0, 0o644)
        log(f'preboot: {relative} staged ({source.stat().st_size} bytes)')
    host.run('sync')


def stage_gpu(data: Path, stager: str, gpu_base: str, log: host.Log) -> None:
    with progress.step('gpu: staging the Metal bundle'):
        result = host.stream([sys.executable, stager, str(data), '--build', str(gpu_base), '--progress'], progress.ours)
    log('gpu:', (result.stdout + result.stderr).strip())
    if result.returncode:
        raise host.HostError('the Metal bundle was not staged')


def merge_tree(source: str | Path, target: Path, owners: ownership.Owners | None, log: host.Log, what: str) -> None:
    """Copy `source` onto `target` (directories merged, files replaced): directories 0755, files 0644, root's."""
    count = 0
    paths = sorted(Path(source).rglob('*'))
    with progress.step(f'{what}: copying', sum(p.stat().st_size for p in paths if not p.is_dir())):
        for path in paths:
            destination = Path(target) / path.relative_to(source)
            if path.is_dir():
                destination.mkdir(exist_ok=True)
                destination.chmod(0o755)
            else:
                shutil.copyfile(path, destination)
                destination.chmod(0o644)
                count += 1
                progress.advance(destination.stat().st_size)
            if owners:
                owners.set(destination, 0, 0, 0o755 if path.is_dir() else 0o644)
    log(f'{what}: {count} files')
