"""Where the tools find their inputs and keep what is not the repository's, run by `bazel run` or by hand."""

import functools
import os
import time
from pathlib import Path

try:
    from python.runfiles import runfiles as rules_python_runfiles
except ImportError:
    rules_python_runfiles = None


@functools.cache
def _runfiles() -> 'rules_python_runfiles.Runfiles | None':
    # Create() parses the whole runfiles manifest (half a megabyte for //:image), so it is read once per process.
    return rules_python_runfiles.Create() if rules_python_runfiles else None


def located(path: str | os.PathLike | None) -> str | None:
    """Return a runfiles path (`bazel run`) or a plain one as an absolute path; None and '' stay as they are."""
    if not path:
        return path
    path = os.fspath(path)
    if not os.path.isabs(path) and _runfiles():
        # Every caller names its runfiles from the main repository: '' says so without a walk up the call stack.
        path = _runfiles().Rlocation(path, '') or path
    return os.path.abspath(path)


def workspace() -> Path:
    """Return the source tree: the one `bazel run` was started in, or the one holding this file."""
    return Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY') or Path(os.path.abspath(__file__)).parents[2])


def state_dir() -> Path:
    """Where the tools keep what is not the repository's: ~/Library/Application Support/switch-ios, or $SWITCH_HOME."""
    return Path(os.environ.get('SWITCH_HOME') or Path.home() / 'Library/Application Support/switch-ios')


def run_log(name: str, folder: str = 'switch-ios') -> Path:
    """Return the log file of one run: ~/Library/Logs/<folder>/<name>-<date>-<time>.log."""
    return Path.home() / 'Library/Logs' / folder / '{}-{}.log'.format(name, time.strftime('%Y%m%d-%H%M%S'))
