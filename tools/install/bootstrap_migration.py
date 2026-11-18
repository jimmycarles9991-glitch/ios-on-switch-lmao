"""Schedule bootstrap package work on the next boot."""

import fcntl
import os
import re
import stat
from contextlib import ExitStack, suppress
from pathlib import Path

from tools.install import host, owners as ownership

DIGEST = re.compile(rb'[0-9a-f]{64}')
PHASE = re.compile(rb'[A-Za-z0-9._-]{1,64}')
PENDING = 'migration.pending'
RECEIPT = 'migration.state'
GUARD = 'system-app-migration'
DIRECTORY = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC


def _check(info: os.stat_result, device: int, name: str, verify_owner: bool, owner: int = 0) -> None:
    if info.st_dev != device or info.st_mode & 0o022 or (verify_owner and (info.st_uid, info.st_gid) != (owner, owner)):
        raise host.HostError(f'bootstrap migration: unsafe ownership, permissions or mount at {name}')


def read_record(directory: int, name: str, limit: int, verify_owner: bool, owner: int = 0) -> bytes | None:
    try:
        fd = os.open(name, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=directory)
    except FileNotFoundError:
        return None
    try:
        info = os.fstat(fd)
        _check(info, os.fstat(directory).st_dev, name, verify_owner, owner)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > limit:
            raise host.HostError(f'bootstrap migration: expected a bounded, independent regular file at {name}')
        with os.fdopen(fd, 'rb', closefd=False) as file:
            data = file.read(limit + 1)
        if len(data) != info.st_size:
            raise host.HostError(f'bootstrap migration: {name} changed while reading')
        return data
    finally:
        os.close(fd)


def _generation(receipt: bytes | None) -> bytes | None:
    if receipt is None:
        return None
    if receipt.endswith(b'\n'):
        fields = receipt[:-1].split(b' ')
        if len(fields) >= 2 and DIGEST.fullmatch(fields[1]):
            if len(fields) == 2 and fields[0] in (b'complete', b'repair'):
                return fields[1]
            if len(fields) in (3, 4) and PHASE.fullmatch(fields[2]):
                if len(fields) == 3 and fields[0] == b'working':
                    return fields[1]
                if (
                    len(fields) == 4
                    and fields[0] == b'failed'
                    and re.fullmatch(rb'[1-9][0-9]{0,9}', fields[3])
                    and int(fields[3]) <= 0x7FFFFFFF
                ):
                    return fields[1]
    raise host.HostError('bootstrap migration: invalid migration.state')


def _publish(base: int, digest: bytes, verify_owner: bool) -> None:
    partial = PENDING + '.partial'
    if read_record(base, partial, 65, verify_owner) is not None:
        os.unlink(partial, dir_fd=base)
    fd = os.open(partial, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644, dir_fd=base)
    try:
        with os.fdopen(fd, 'wb') as file:
            file.write(digest + b'\n')
            file.flush()
            os.fchmod(file.fileno(), 0o644)
            if verify_owner:
                os.fchown(file.fileno(), 0, 0)
            os.fsync(file.fileno())
        os.replace(partial, PENDING, src_dir_fd=base, dst_dir_fd=base)
    finally:
        with suppress(FileNotFoundError):
            os.unlink(partial, dir_fd=base)
    os.fsync(base)


def _rearm_native(data: int, switch: int, verify_owner: bool) -> bool:
    if read_record(switch, GUARD, 256, verify_owner) is None:
        return False
    try:
        migrator = os.open('datamigrator', DIRECTORY, dir_fd=data)
    except FileNotFoundError:
        return False
    try:
        _check(os.fstat(migrator), os.fstat(data).st_dev, 'datamigrator', verify_owner, 501)
        if read_record(migrator, 'migrate', 0, verify_owner, 501) is None:
            return False
    finally:
        os.close(migrator)
    # With the guard set, early boot drops Apple's unfinished migration request.
    os.unlink(GUARD, dir_fd=switch)
    os.fsync(switch)
    return True


def schedule(data: Path, build: str, generation: str | None, log: host.Log, owners: ownership.Owners | None) -> bool:
    if generation is None:
        return False
    digest = generation.encode('ascii')
    if not DIGEST.fullmatch(digest) or not re.fullmatch(r'[A-Za-z0-9]+', build):
        raise host.HostError('bootstrap migration: invalid build or managed generation')
    verify_owner = isinstance(owners, ownership.Immediate)
    with ExitStack() as stack:
        parent = os.open(data, DIRECTORY)
        stack.callback(os.close, parent)
        data_info = os.fstat(parent)
        device = data_info.st_dev
        _check(data_info, device, 'Data', verify_owner)

        def directory(child: str, parent: int) -> int:
            fd = os.open(child, DIRECTORY, dir_fd=parent)
            stack.callback(os.close, fd)
            _check(os.fstat(fd), device, child, verify_owner)
            return fd

        switch = directory('Switch', parent)
        base = directory(build, directory('bootstrap', switch))
        fcntl.flock(base, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Deploy's noowners mount can compare receipts for scheduling only. The worker checks owners on real Data.
        receipt = read_record(base, RECEIPT, 159, verify_owner)
        interrupted = receipt == b'working ' + digest + b' native-migration\n'
        if _generation(receipt) == digest and not interrupted:
            return False
        pending = read_record(base, PENDING, 65, verify_owner)
        if pending is not None and (
            len(pending) != 65 or not pending.endswith(b'\n') or not DIGEST.fullmatch(pending[:-1])
        ):
            raise host.HostError('bootstrap migration: invalid migration.pending')
        if interrupted:
            rearmed = pending == digest + b'\n' and _rearm_native(parent, switch, verify_owner)
            if rearmed:
                log(f'bootstrap: interrupted native migration rearmed for {build}')
            return rearmed
        if pending == digest + b'\n':
            return False
        _publish(base, digest, verify_owner)
    if isinstance(owners, ownership.Journal):
        owners.set(data / 'Switch/bootstrap' / build / PENDING, 0, 0, 0o644)
    log(f'bootstrap: managed package migration requested for {build}')
    return True
