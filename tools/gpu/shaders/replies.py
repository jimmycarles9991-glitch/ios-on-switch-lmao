"""Validate retained compiler requests and NSC1 replies without changing their lookup keys."""

import hashlib
import json
import os
import re
import struct
import tempfile
from pathlib import Path

MAGIC = b'NSC1'
HEADER = 48
REQUEST_LIMIT = 64 << 20
NAME = re.compile(r'[0-9a-f]{64}\.reply')
UUID = re.compile(r'[0-9a-f]{32}')
REQUEST_NAME = re.compile(r'[0-9a-f]{64}\.request')
TEMPORARY_NAME = re.compile(r'(?:[0-9a-f]{64}\.reply\.[0-9]+\.tmp|\.reply-[A-Za-z0-9_-]+)')
COMPONENT = re.compile(r'[0-9A-Za-z_.-]{1,64}')


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def payload(data: bytes) -> bytes | None:
    if len(data) < HEADER or data[:4] != MAGIC:
        return None
    warnings, length = struct.unpack_from('<IQ', data, 4)
    if not length or HEADER + warnings + length != len(data):
        return None
    if hashlib.sha256(data[HEADER:]).digest() != data[16:HEADER]:
        return None
    return data[HEADER + warnings :]


def reply_ok(data: bytes) -> bool:
    return payload(data) is not None


def _component(value: object) -> bool:
    return isinstance(value, str) and COMPONENT.fullmatch(value) is not None and value not in ('.', '..')


def _unique_fields(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate provenance field: ' + key)
        result[key] = value
    return result


def _identity(root: Path, path: Path) -> dict:
    record = json.loads(path.read_text(), object_pairs_hook=_unique_fields)
    if not isinstance(record, dict) or type(record.get('schema')) is not int or record['schema'] != 1:
        raise ValueError('unsupported provenance schema')
    name, os_build = record.get('reply'), record.get('osBuild')
    if not isinstance(name, str) or not NAME.fullmatch(name) or not _component(os_build):
        raise ValueError('invalid reply or OS build')
    if path.relative_to(root) != Path('provenance') / os_build / (name[:-6] + '.json'):
        raise ValueError('provenance path does not match its reply and OS')
    if not _component(record.get('targetBuild')):
        raise ValueError('invalid target build')
    uuid, kind = record.get('compilerUUID'), record.get('requestType')
    if not isinstance(uuid, str) or not UUID.fullmatch(uuid) or uuid == '0' * 32:
        raise ValueError('invalid compiler UUID')
    if type(kind) is not int or not 0 <= kind <= 0xFFFFFFFF:
        raise ValueError('invalid request type')
    return record


def _request(root: Path, record: dict) -> Path:
    length = record.get('requestBytes')
    if type(length) is not int or not 0 <= length <= REQUEST_LIMIT:
        raise ValueError('request size exceeds capture bound')
    request_path = root / (record['reply'][:-6] + '.request')
    if request_path.stat().st_size != length:
        raise ValueError('request length does not match provenance')
    request = request_path.read_bytes()
    if sha(request) != record.get('requestSHA256'):
        raise ValueError('request hash does not match provenance')
    key = sha(bytes.fromhex(record['compilerUUID']) + struct.pack('<I', record['requestType']) + request)
    if record['reply'] != key + '.reply':
        raise ValueError('request and compiler do not reproduce reply key')
    return request_path


def _record(root: Path, path: Path, entries: dict) -> tuple[dict, Path]:
    record = _identity(root, path)
    request_path = _request(root, record)
    reply = entries.get(record['reply'])
    if not reply or record.get('replySHA256') != reply['sha256']:
        raise ValueError('reply hash does not match provenance')
    if record.get('metallibSHA256') != reply['metallibSHA256']:
        raise ValueError('output library hash does not match reply')
    return record, request_path


def _entries(root: Path, paths: list[Path]) -> tuple[dict, dict, list]:
    files, entries, refused = {}, {}, []
    for path in paths:
        if path.parent != root or path.suffix != '.reply':
            continue
        data = path.read_bytes()
        output = payload(data)
        if not NAME.fullmatch(path.name) or output is None:
            refused.append({'path': path.name, 'reason': 'invalid reply name or envelope'})
            continue
        files[path.name] = sha(data)
        entries[path.name] = {'sha256': sha(data), 'metallibSHA256': sha(output), 'provenance': []}
    return files, entries, refused


def _provenance(root: Path, paths: list[Path], files: dict, entries: dict) -> list:
    refused = []
    for path in paths:
        relative = path.relative_to(root).as_posix()
        if not relative.startswith('provenance/') or TEMPORARY_NAME.fullmatch(path.name):
            continue
        try:
            record, request_path = _record(root, path, entries)
            entries[record['reply']]['provenance'].append(record)
            files[relative] = sha(path.read_bytes())
            files[request_path.name] = sha(request_path.read_bytes())
        except (OSError, ValueError, TypeError, KeyError) as error:
            refused.append({'path': relative, 'reason': str(error)})
    return refused


def _per_os(entries: dict) -> dict:
    result = {}
    for entry in entries.values():
        for record in entry['provenance']:
            row = result.setdefault(record['osBuild'], {'requests': 0, 'compilerUUIDs': set(), 'targetBuilds': set()})
            row['requests'] += 1
            row['compilerUUIDs'].add(record['compilerUUID'])
            row['targetBuilds'].add(record['targetBuild'])
    for row in result.values():
        row['compilerUUIDs'] = sorted(row['compilerUUIDs'])
        row['targetBuilds'] = sorted(row['targetBuilds'])
    return result


def _declared(paths: list[Path], expected: list[Path] | None) -> list:
    if expected is None:
        return []
    declared = set(expected)
    return [
        *({'path': str(p), 'reason': 'declared input missing'} for p in sorted(declared - set(paths))),
        *({'path': str(p), 'reason': 'input absent from declared inventory'} for p in sorted(set(paths) - declared)),
    ]


def _pending(root: Path, paths: list[Path], files: dict) -> tuple[list, list]:
    pending, refused = [], []
    for path in paths:
        relative = path.relative_to(root).as_posix()
        if relative in files or (path.parent == root and path.suffix == '.reply'):
            continue
        request = path.parent == root and REQUEST_NAME.fullmatch(path.name)
        temporary = TEMPORARY_NAME.fullmatch(path.name)
        if request or temporary:
            if path.stat().st_size > REQUEST_LIMIT:
                refused.append({'path': relative, 'reason': 'pending capture exceeds size bound'})
                continue
            data = path.read_bytes()
            files[relative] = sha(data)
            pending.append(
                {
                    'path': relative,
                    'sha256': sha(data),
                    'bytes': len(data),
                    'kind': 'uncommitted-request' if request else 'interrupted-write',
                }
            )
        elif not relative.startswith('provenance/'):
            refused.append({'path': relative, 'reason': 'unknown input'})
    return pending, refused


def inventory(root: Path | None, expected: list[Path] | None = None, strict: bool = False) -> dict:
    paths = sorted(p for p in root.rglob('*') if p.is_file()) if root and root.is_dir() else []
    files, entries, refused = _entries(root, paths)
    refused.extend(_declared(paths, expected))
    refused.extend(_provenance(root, paths, files, entries))
    pending, incomplete = _pending(root, paths, files)
    refused.extend(incomplete)
    result = {
        'schema': 1,
        'scope': 'captured-successful-compiler-requests',
        'unseenRequests': 'unknown',
        'inputComplete': not refused,
        'replies': len(entries),
        'attributedReplies': sum(bool(row['provenance']) for row in entries.values()),
        'unattributedReplies': sum(not row['provenance'] for row in entries.values()),
        'perOS': _per_os(entries),
        'pending': pending,
        'entries': entries,
        'files': files,
        'refused': refused,
    }
    if strict and refused:
        raise ValueError('compiler reply inventory refused: ' + json.dumps(refused, sort_keys=True))
    return result


def copy_inventory(source: Path, target: Path, report: dict) -> int:
    staged = []
    ordered = sorted(report['files'].items(), key=lambda item: (item[0].endswith('.json'), item[0]))
    for relative, digest in ordered:
        path = target / relative
        data = (source / relative).read_bytes()
        if sha(data) != digest:
            raise ValueError(f'compiler reply input changed: {relative}')
        if path.exists():
            if path.read_bytes() != data:
                raise ValueError(f'conflicting compiler reply input: {relative}')
        else:
            staged.append((path, data))
    for path, data in staged:
        path.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary = tempfile.mkstemp(prefix='.reply-', dir=path.parent)
        try:
            with os.fdopen(fd, 'wb') as output:
                output.write(data)
            os.replace(temporary, path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
    return sum(path.suffix == '.reply' for path, _ in staged)


def verify(root: Path, expected: dict) -> None:
    actual = inventory(root, strict=True)
    if actual['files'] != expected['files']:
        raise ValueError('installed compiler replies differ from the input inventory')
