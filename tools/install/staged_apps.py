import filecmp
import json
import os
import plistlib
import re
import shutil
from pathlib import Path
from typing import NamedTuple

from tools.common import seed_manifest
from tools.install import card, host, owners as ownership

SEEDS = 'Switch/overlay/usr/share/Switch/Applications'
STAGED = 'staged_system_apps'
INSTALLED = 'containers/Bundle/Application'
STATES = 'installd/Library/MobileInstallation/BackedUpState/SystemAppInstallState.plist'
PLACEHOLDERS = 'Switch/overlay/System/Library/AppPlaceholders'
SIGNATURES = 'Switch/overlay/System/Library/AppSignatures'


def _info(path: Path) -> dict:
    try:
        info = plistlib.loads(path.read_bytes())
    except FileNotFoundError:
        return {}
    except plistlib.InvalidFileException as error:
        raise host.HostError(f'invalid app metadata: {path}') from error
    if not isinstance(info, dict):
        raise host.HostError(f'app metadata is not a dictionary: {path}')
    return info


def _version(value: str) -> tuple[int, ...]:
    if not isinstance(value, str) or not re.fullmatch(r'\d+(?:\.\d+)*', value):
        raise host.HostError(f'invalid staged app version: {value!r}')
    parts = tuple(int(part) for part in value.split('.'))
    while parts and parts[-1] == 0:
        parts = parts[:-1]
    return parts


def _matches(source: Path, target: Path) -> bool:
    if not target.is_dir() or target.is_symlink():
        return False
    wanted = {p.relative_to(source): info for p, info in ownership.walk(source)}
    actual = {p.relative_to(target): info for p, info in ownership.walk(target)}
    if wanted.keys() != actual.keys():
        return False
    for path, info in wanted.items():
        if info.st_mode != actual[path].st_mode:
            return False
        left, right = source / path, target / path
        if left.is_symlink():
            if os.readlink(left) != os.readlink(right):
                return False
        elif left.is_file() and not filecmp.cmp(left, right, shallow=False):
            return False
    return True


def _remove(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


def _installed(data: Path) -> dict:
    installed = {}
    for app in (data / INSTALLED).glob('*/*.app'):
        info = _info(app / 'Info.plist')
        executable = info.get('CFBundleExecutable')
        if executable and (app / executable).is_file():
            installed[info.get('CFBundleIdentifier')] = info
    return installed


def _mobile_tree(path: Path) -> bool:
    return all((info.st_uid, info.st_gid) == (501, 501) for _, info in ownership.walk(path))


def _check_embedded_profile(app: Path) -> None:
    profile = app / 'embedded.mobileprovision'
    if profile.exists():
        try:
            metadata = plistlib.loads(host.run('/usr/bin/security', 'cms', '-D', '-i', profile, binary=True))
        except (host.HostError, plistlib.InvalidFileException) as error:
            raise host.HostError(f'{app.name}: embedded provisioning profile is not a decodable CMS profile') from error
        if not isinstance(metadata, dict) or not isinstance(metadata.get('Entitlements'), dict):
            raise host.HostError(f'{app.name}: embedded provisioning profile has no entitlements dictionary')


class _Seeds(NamedTuple):
    overlay: Path
    installed: dict
    states: dict
    pin: dict | None = None

    @classmethod
    def read(cls, data: Path, overlay: Path, package: Path | None = None) -> '_Seeds':
        pin = None
        if package is not None:
            manifest = package / seed_manifest.MANIFEST
            app = overlay / 'usr/share/Switch/Applications' / seed_manifest.APP
            if manifest.exists() or app.exists():
                try:
                    pin = json.loads(manifest.read_bytes())
                    if not isinstance(pin, dict) or not app.is_dir():
                        raise ValueError('missing Installer seed or invalid manifest')
                except (OSError, ValueError) as error:
                    raise host.HostError(f'cannot verify Installer seed from {manifest}: {error}') from error
        return cls(overlay, _installed(data), _info(data / STATES), pin)


def _verify_pinned(app: Path, pin: dict | None) -> None:
    if pin is not None:
        try:
            seed_manifest.verify(app, pin)
        except (KeyError, TypeError, ValueError) as error:
            raise host.HostError(str(error)) from error


def _copy_seed(source: Path, target: Path, pin: dict | None, *, inherited: bool) -> bool:
    if _matches(source, target) and (not inherited or _mobile_tree(target)):
        _verify_pinned(target, pin)
        return False
    partial = target.with_name('.' + target.name + '.partial')
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copytree(source, partial, symlinks=True, copy_function=shutil.copy)
        _verify_pinned(partial, pin)
        if inherited and not _mobile_tree(partial):
            raise host.HostError(f'staged app did not inherit mobile ownership: {source.name}')
        _remove(target)
        partial.rename(target)
    finally:
        _remove(partial)
    return True


def _stage(
    data: Path, seeds: _Seeds, owners: ownership.Owners | None, log: host.Log, *, inherited: bool = False
) -> bool:
    overlay, installed, states, pin = seeds
    pending = False
    for source in sorted((overlay / 'usr/share/Switch/Applications').glob('*.app')):
        info = _info(source / 'Info.plist')
        identifier, executable = info.get('CFBundleIdentifier'), info.get('CFBundleExecutable')
        selected_pin = pin if source.name == seed_manifest.APP or identifier == seed_manifest.IDENTIFIER else None
        _verify_pinned(source, selected_pin)
        if not identifier or not executable or not (source / executable).is_file():
            raise host.HostError(f'incomplete migration seed: {source}')
        version = _version(info.get('CFBundleVersion'))
        placeholder = _info(overlay / 'System/Library/AppPlaceholders' / source.name / 'Info.plist')
        if any(placeholder.get(key) != info.get(key) for key in ('CFBundleIdentifier', 'CFBundleVersion')):
            raise host.HostError(f'migration seed has no matching system app placeholder: {source}')
        if not (overlay / 'System/Library/AppSignatures' / identifier).is_file():
            raise host.HostError(f'migration seed has no detached signature: {source}')
        target = data / STAGED / source.name
        partial = target.with_name('.' + target.name + '.partial')
        current = installed.get(identifier)
        if (current and _version(current.get('CFBundleVersion')) >= version) or (
            not current and states.get(identifier) == 2
        ):
            _remove(partial)
            _remove(target)
            continue
        _check_embedded_profile(source)
        _remove(partial)
        pending = True
        if _copy_seed(source, target, selected_pin, inherited=inherited):
            log(f'system app migration: staged {identifier} ({info["CFBundleVersion"]})')
        if owners:
            owners.set(target.parent, 501, 501, 0o755)
            for path, _ in ownership.walk(target):
                owners.set(path, 501, 501)
    return pending


def stage(data: Path, owners: ownership.Owners | None, log: host.Log, *, package: Path) -> bool:
    return _stage(data, _Seeds.read(data, data / 'Switch/overlay', package), owners, log)


def stage_on_card(identifier: str, package: Path, log: host.Log) -> bool:
    overlay = package / 'overlay'
    if not (package / seed_manifest.MANIFEST).exists() and not any(
        (overlay / 'usr/share/Switch/Applications').glob('*.app')
    ):
        return False
    with card.mounted(identifier, log, read_only=True) as data:
        seeds = _Seeds.read(data, overlay, package)
    with card.mounted(identifier, log, owners=True) as data:
        parent = data / STAGED
        if not parent.is_dir() or parent.is_symlink() or (parent.stat().st_uid, parent.stat().st_gid) != (501, 501):
            raise host.HostError(f'{parent}: expected the mobile staging directory')
        return _stage(data, seeds, None, log, inherited=True)
