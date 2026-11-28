"""External, idempotent SSH authorized_keys staging for development images.

Public keys are read only at deploy time (never by Bazel actions), normalized, sorted, deduplicated and staged at
<Data>/Switch/ssh/authorized_keys for development boot packages only. Release deploys remove any stale file.
"""

import base64
import binascii
import json
import os
import re
import struct
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path

from tools.common import files

ENV_VAR = 'SWITCH_SSH_AUTHORIZED_KEYS'
STAGED_DIR = 'Switch/ssh'
STAGED = 'Switch/ssh/authorized_keys'
OVERLAY_DEB = 'overlay/usr/share/Switch/bootstrap/packages/switch-ssh.deb'
KEY_TYPES = frozenset(
    {
        'ssh-ed25519',
        'ssh-rsa',
        'ssh-dss',
        'ecdsa-sha2-nistp256',
        'ecdsa-sha2-nistp384',
        'ecdsa-sha2-nistp521',
        'sk-ssh-ed25519@openssh.com',
        'sk-ecdsa-sha2-nistp256@openssh.com',
    }
)
_PRIVATE_BEGIN = re.compile(r'^-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----$')
_PRIVATE_END = re.compile(r'^-----END [A-Z0-9 ]*PRIVATE KEY-----$')


@dataclass(frozen=True)
class Options:
    path: str | Path | None = None
    disabled: bool = False
    env: Mapping[str, str] | None = None
    home: Path | None = None


@dataclass(frozen=True)
class KeyPlan:
    development: bool
    disabled: bool
    keys: tuple[str, ...]
    sources: tuple[Path, ...]

    @property
    def content(self) -> bytes:
        if not self.development or self.disabled or not self.keys:
            return b''
        return ('\n'.join(self.keys) + '\n').encode()


def is_development_package(boot_package: Path) -> bool:
    """Return whether `boot_package` is a development image carrying switch-ssh."""
    root = Path(boot_package)
    if (root / OVERLAY_DEB).is_file():
        return True
    manifest = root / 'bootstrap/manifest.json'
    if not manifest.is_file():
        return False
    try:
        data = json.loads(manifest.read_text())
    except (OSError, ValueError):
        return False
    return isinstance(data, dict) and (
        'ssh' in data or (isinstance(data.get('managed_packages'), dict) and 'switch-ssh' in data['managed_packages'])
    )


def _valid_key_blob(key_type: str, blob: str) -> bool:
    try:
        raw = base64.b64decode(blob, validate=True)
    except (binascii.Error, ValueError):
        return False
    header = 4 + len(key_type)
    if len(raw) <= header:
        return False
    (length,) = struct.unpack_from('>I', raw, 0)
    return length == len(key_type) and raw[4:header] == key_type.encode('ascii')


def _sanitize_comment(comment: str) -> str:
    cleaned = ''.join(ch if 0x20 <= ord(ch) <= 0x7E else ' ' for ch in comment)
    return ' '.join(cleaned.split())


def parse_public_keys(text: str) -> list[str]:
    """Parse, validate, deduplicate and deterministically sort OpenSSH public keys from `text`."""
    found: dict[tuple[str, str], str] = {}
    in_private = False
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if _PRIVATE_BEGIN.match(line):
            in_private = True
            continue
        if in_private:
            if _PRIVATE_END.match(line):
                in_private = False
            continue
        parts = line.split(None, 2)
        if len(parts) < 2:
            continue
        key_type, blob = parts[0], parts[1]
        if key_type not in KEY_TYPES or not _valid_key_blob(key_type, blob):
            continue
        comment = _sanitize_comment(parts[2]) if len(parts) == 3 else ''
        canonical = f'{key_type} {blob} {comment}' if comment else f'{key_type} {blob}'
        identity = (key_type, blob)
        if identity not in found or canonical < found[identity]:
            found[identity] = canonical
    return [found[key] for key in sorted(found)]


def _resolve_explicit(raw: str | Path, env: Mapping[str, str]) -> Path:
    path = Path(raw).expanduser()
    if not path.is_absolute() and env.get('BUILD_WORKING_DIRECTORY'):
        path = Path(env['BUILD_WORKING_DIRECTORY']) / path
    return path


def resolve_sources(options: Options | None = None) -> list[Path]:
    """Return external public key files from --ssh-authorized-keys > $SWITCH_SSH_AUTHORIZED_KEYS > ~/.ssh/*.pub."""
    opts = options or Options()
    env = os.environ if opts.env is None else opts.env
    home = Path.home() if opts.home is None else opts.home
    explicit = opts.path if opts.path is not None else (env.get(ENV_VAR, '').strip() or None)
    if explicit is not None:
        path = _resolve_explicit(explicit, env)
        if not path.exists():
            raise ValueError(f'SSH authorized keys path not found: {path}')
        if path.is_dir():
            return sorted(item for item in path.glob('*.pub') if item.is_file())
        if path.is_file():
            return [path]
        raise ValueError(f'SSH authorized keys path is not a file or directory: {path}')
    ssh_dir = home / '.ssh'
    if not ssh_dir.is_dir():
        return []
    return sorted(item for item in ssh_dir.glob('*.pub') if item.is_file())


def load(boot_package: Path, log: Callable[..., None], options: Options | None = None) -> KeyPlan:
    """Resolve and validate public keys to stage for `boot_package`."""
    opts = options or Options()
    development = is_development_package(boot_package)
    if not development or opts.disabled:
        return KeyPlan(development=development, disabled=opts.disabled, keys=(), sources=())
    sources = resolve_sources(opts)
    chunks = []
    for path in sources:
        text = path.read_text(encoding='utf-8', errors='replace')
        if 'PRIVATE KEY-----' in text:
            log(f'ssh authorized_keys: skipped private key material in {path}')
        chunks.append(text)
    keys = tuple(parse_public_keys('\n'.join(chunks)))
    if not keys:
        log('ssh authorized_keys: no valid public keys found (sshd will refuse logins)')
    return KeyPlan(development=True, disabled=False, keys=keys, sources=tuple(sources))


def _check_real_dir(path: Path) -> None:
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        raise OSError(f'ssh authorized_keys: {path} is not a real directory')


def _remove_stale(parent: Path, target: Path, reason: str, log: Callable[..., None], dry_run: bool) -> bool:
    if not target.exists() and not target.is_symlink():
        return False
    if not dry_run:
        target.unlink()
        if parent.is_dir() and not parent.is_symlink() and not any(parent.iterdir()):
            parent.rmdir()
    log('ssh authorized_keys: removed stale {} ({}{})'.format(STAGED, reason, ', dry run' if dry_run else ''))
    return True


def stage(data_root: Path, plan: KeyPlan, log: Callable[..., None], dry_run: bool = False) -> bool:
    """Stage or remove <Data>/Switch/ssh/authorized_keys idempotently; return whether anything changed."""
    root = Path(data_root)
    switch_dir = root / 'Switch'
    parent = root / STAGED_DIR
    target = root / STAGED
    _check_real_dir(switch_dir)
    _check_real_dir(parent)
    content = plan.content
    if not content:
        reason = 'release image' if not plan.development else ('disabled' if plan.disabled else 'no valid keys')
        return _remove_stale(parent, target, reason, log, dry_run)
    if target.is_file() and not target.is_symlink():
        mode = target.stat().st_mode & 0o777
        if target.read_bytes() == content and mode == 0o600:
            log(f'ssh authorized_keys: unchanged ({len(plan.keys)} key(s))')
            return False
    if not dry_run:
        switch_dir.mkdir(exist_ok=True)
        parent.mkdir(exist_ok=True)
        parent.chmod(0o700)
        if target.is_symlink():
            target.unlink()
        files.write_bytes(target, content, mode=0o600)
    log('ssh authorized_keys: staged {} key(s) -> {}{}'.format(len(plan.keys), STAGED, ' (dry run)' if dry_run else ''))
    return True


def plan_description(plan: KeyPlan) -> str:
    """Return a single-line deploy plan description for `plan`."""
    if not plan.development:
        return 'SSH authorized_keys: remove any staged keys (release image)'
    if plan.disabled:
        return 'SSH authorized_keys: remove any staged keys (--no-ssh-authorized-keys)'
    if not plan.keys:
        return 'SSH authorized_keys: no keys found (remove any staged keys)'
    return f'SSH authorized_keys: stage {len(plan.keys)} key(s)'
