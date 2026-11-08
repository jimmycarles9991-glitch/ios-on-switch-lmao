"""System apps the overlay adds under /Applications, registered the way iOS registers an OS update's.

LaunchServices scans the system app directories only during datamigrator's upgrade migration
(00LaunchServicesMigrator.migrator's populateLSDatabase), which runs when the build recorded in com.apple.migration
differs from the running one, or when /private/var/datamigrator/migrate exists: the sentinel datamigrator's own
forceMigrationOnNextReboot writes and removes once the migration has run. The deploy writes that sentinel when the
overlay's /Applications changed, so the next boot registers the new bundles and drops the removed ones.
"""

from pathlib import Path

from tools.install import card as cards, host, renamed

SENTINEL = 'datamigrator/migrate'
APPLICATIONS = 'overlay/Applications'


def digest(data_root: Path) -> str | None:
    return cards.tree_digest(Path(data_root) / renamed.DATA_FOLDER / APPLICATIONS)


def request_migration(data_root: Path, before: str | None, log: host.Log) -> bool:
    """Write the migration sentinel when the overlay's apps differ from `before` (their digest ahead of the sync)."""
    if digest(data_root) == before:
        return False
    sentinel = Path(data_root) / SENTINEL
    sentinel.parent.mkdir(parents=True, exist_ok=True)
    sentinel.touch()
    log(f'system apps changed: upgrade migration on the next boot (<Data>/{SENTINEL})')
    return True
