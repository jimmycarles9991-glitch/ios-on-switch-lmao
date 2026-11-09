"""The commit a deploy wrote to the card, <Data>/Switch/deployed.json, which tools/hwtest joins HW Test's reports with.

The build cannot stamp a commit (.bazelrc: --nostamp keeps every output reproducible) and the app's sandbox cannot read
this folder, so the deploy records the checkout `bazel run` started in, and the Mac reads it back off the card.
"""

import json
import subprocess
from collections.abc import Callable
from datetime import UTC, datetime
from pathlib import Path

from tools.install import renamed

NAME = 'deployed.json'


def describe(checkout: Path) -> dict:
    """Return the checkout's HEAD, branch and whether tracked files differ ('' and False outside a git checkout)."""

    def git(*args: str) -> str:
        result = subprocess.run(['git', '-C', str(checkout), *args], capture_output=True, text=True, check=False)
        return result.stdout.strip() if result.returncode == 0 else ''

    return {
        'commit': git('rev-parse', 'HEAD'),
        'branch': git('rev-parse', '--abbrev-ref', 'HEAD'),
        'dirty': bool(git('status', '--porcelain', '--untracked-files=no')),
    }


def record(data_root: Path, checkout: Path, build: str, log: Callable[..., None]) -> dict:
    """Write deployed.json into the Data volume's own folder; return what was written."""
    info = {**describe(checkout), 'build': build, 'deployed_at': datetime.now(UTC).isoformat(timespec='seconds')}
    folder = renamed.data_folder(data_root)
    folder.mkdir(parents=True, exist_ok=True)
    (folder / NAME).write_text(json.dumps(info, indent=1) + '\n')
    log('deployed: {}{} -> {}'.format(info['commit'][:12] or '?', ' (dirty)' if info['dirty'] else '', NAME))
    return info
