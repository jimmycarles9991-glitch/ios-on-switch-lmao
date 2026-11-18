import copy
import ctypes
import gzip
import hashlib
import io
import posixpath
import tarfile
from pathlib import Path, PurePosixPath

PREFIX = 'var/jb'
MAX_FILE = 256 * 1024 * 1024


def read_deb(path: Path) -> dict[str, bytes]:
    data = path.read_bytes()
    if data[:8] != b'!<arch>\n':
        raise ValueError('invalid Debian archive')
    result = {}
    offset = 8
    while offset < len(data):
        header = data[offset : offset + 60]
        if len(header) != 60 or header[58:] != b'`\n':
            raise ValueError('invalid ar member header')
        name = header[:16].decode('ascii').strip().removesuffix('/')
        length = int(header[48:58])
        offset += 60
        if length < 0 or offset + length > len(data) or name in result:
            raise ValueError('invalid ar member extent or duplicate')
        result[name] = data[offset : offset + length]
        offset += length + length % 2
    if offset != len(data) or result.get('debian-binary') != b'2.0\n':
        raise ValueError('unsupported Debian archive')
    return result


def tar_bytes(entries: dict, compression: str = '', *, tar_format: int = tarfile.PAX_FORMAT) -> bytes:
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w', format=tar_format) as archive:
        for name, (metadata, data) in sorted(entries.items()):
            normalized = copy.copy(metadata)
            normalized.name, normalized.mtime = name, 0
            normalized.uname = normalized.gname = ''
            normalized.pax_headers = {}
            archive.addfile(normalized, io.BytesIO(data) if normalized.isfile() else None)
    data = out.getvalue()
    return gzip.compress(data, mtime=0) if compression == ':gz' else data


def write_deb(path: Path, control: dict, entries: dict) -> None:
    control = dict(control)
    checksums = [
        hashlib.md5(data).hexdigest() + '  ' + name
        for name, (member, data) in sorted(entries.items())
        if member.isfile()
    ]
    add_file(control, 'md5sums', ('\n'.join(checksums) + '\n').encode())
    # Procursus dpkg handles GNU long names but rejects PAX extended headers.
    members = {
        'debian-binary': b'2.0\n',
        'control.tar.gz': tar_bytes(control, ':gz', tar_format=tarfile.GNU_FORMAT),
        'data.tar.gz': tar_bytes(entries, ':gz', tar_format=tarfile.GNU_FORMAT),
    }
    with path.open('wb') as output:
        output.write(b'!<arch>\n')
        for name, data in members.items():
            output.write(f'{name + "/":<16}{0:<12}{0:<6}{0:<6}{"100644":<8}{len(data):<10}`\n'.encode())
            output.write(data)
            if len(data) % 2:
                output.write(b'\n')


def member_name(member: tarfile.TarInfo, strip_prefix: bool) -> str | None:
    parts = PurePosixPath(member.name).parts
    if '..' in parts or member.name.startswith('/'):
        raise ValueError('unsafe archive path')
    name = str(PurePosixPath(*parts))
    if name in ('.', 'var', PREFIX):
        return None
    if strip_prefix:
        if not name.startswith(PREFIX + '/'):
            raise ValueError(f'archive file outside /var/jb: {name}')
        name = name[len(PREFIX) + 1 :]
    return name


def check_member(member: tarfile.TarInfo, name: str) -> None:
    if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
        raise ValueError(f'unsupported archive entry: {name}')
    if member.size < 0 or member.size > MAX_FILE:
        raise ValueError('oversized archive entry')
    if not member.issym():
        return
    if name == 'etc/localtime' and member.linkname == '/var/db/timezone/localtime':
        return
    if member.linkname.startswith('/var/jb/'):
        target = member.linkname[len('/var/jb/') :]
    elif member.linkname.startswith('/'):
        raise ValueError(f'external bootstrap link: {name}')
    else:
        target = posixpath.normpath(posixpath.join(posixpath.dirname(name), member.linkname))
    target = posixpath.normpath(target)
    if target == '..' or target.startswith('../'):
        raise ValueError(f'escaping bootstrap link: {name}')


def read_tar(archive: tarfile.TarFile, strip_prefix: bool = True, omit: tuple[str, ...] = ()) -> dict:
    entries = {}
    for member in archive:
        if member.name.removeprefix('./') in omit:
            continue
        name = member_name(member, strip_prefix)
        if name is None:
            continue
        check_member(member, name)
        if name in entries:
            raise ValueError('duplicate archive path')
        stream = archive.extractfile(member) if member.isfile() else None
        entries[name] = member, stream.read() if stream else b''
    materialize_links(entries)
    return entries


def materialize_links(entries: dict) -> None:
    for name, (member, _) in list(entries.items()):
        for parent in PurePosixPath(name).parents:
            if str(parent) in entries and entries[str(parent)][0].issym():
                raise ValueError('archive writes through a symlink')
        if member.islnk():
            target = member.linkname.removeprefix('./').removeprefix(PREFIX + '/')
            if target not in entries or not entries[target][0].isfile():
                raise ValueError('hardlink target is missing or not a regular file')
            regular = copy.copy(entries[target][0])
            regular.name = name
            entries[name] = regular, entries[target][1]


def decode_zstd(data: bytes, library: Path) -> bytes:
    codec = ctypes.CDLL(str(library))
    codec.ZSTD_getFrameContentSize.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    codec.ZSTD_getFrameContentSize.restype = ctypes.c_ulonglong
    codec.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    codec.ZSTD_decompress.restype = ctypes.c_size_t
    codec.ZSTD_isError.argtypes = [ctypes.c_size_t]
    codec.ZSTD_isError.restype = ctypes.c_uint
    size = codec.ZSTD_getFrameContentSize(data, len(data))
    if not 0 < size <= 512 * 1024 * 1024:
        raise ValueError('bootstrap requires a bounded zstd frame content size')
    output = ctypes.create_string_buffer(size)
    decoded = codec.ZSTD_decompress(output, size, data, len(data))
    if codec.ZSTD_isError(decoded) or decoded != size:
        raise ValueError('bootstrap zstd frame failed decompression')
    return output.raw


def read_bootstrap(path: Path, library: Path) -> dict:
    data = decode_zstd(path.read_bytes(), library)
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        return read_tar(archive)


def add_file(entries: dict, name: str, data: bytes, mode: int = 0o644) -> None:
    member = tarfile.TarInfo(name)
    member.mode, member.size = mode, len(data)
    entries[name] = member, data


def add_link(entries: dict, name: str, target: str) -> None:
    member = tarfile.TarInfo(name)
    member.type, member.linkname, member.mode = tarfile.SYMTYPE, target, 0o777
    entries[name] = member, b''


def ensure_directories(entries: dict, boundary: str = '') -> None:
    for name in list(entries):
        for parent in PurePosixPath(name).parents:
            directory = str(parent)
            if directory == '.' or (boundary and not directory.startswith(boundary + '/')):
                continue
            if directory in entries:
                if not entries[directory][0].isdir():
                    raise ValueError('package writes through a non-directory')
                continue
            member = tarfile.TarInfo(directory)
            member.type, member.mode = tarfile.DIRTYPE, 0o755
            entries[directory] = member, b''


def register_package(entries: dict, control: dict, files: dict) -> str:
    ensure_directories(files, boundary=PREFIX)
    stanza = control['control'][1].decode().strip()
    fields = dict(line.split(': ', 1) for line in stanza.splitlines() if ': ' in line and not line.startswith(' '))
    package = fields['Package']
    if fields.get('Architecture') != 'iphoneos-arm64':
        raise ValueError('bootstrap package must be rootless iphoneos-arm64')
    status = entries['Library/dpkg/status'][1].decode()
    status = '\n\n'.join(s for s in status.split('\n\n') if not s.startswith(f'Package: {package}\n'))
    status = status.strip() + '\n\n' + stanza + '\nStatus: hold ok installed\n\n'
    add_file(entries, 'Library/dpkg/status', status.encode())
    names, checksums = [], []
    for name, entry in sorted(files.items()):
        relative = name.removeprefix(PREFIX + '/')
        if relative == name:
            raise ValueError('package writes outside bootstrap')
        entries[relative] = entry
        names.append('/' + name)
        if entry[0].isfile():
            checksums.append(hashlib.md5(entry[1]).hexdigest() + '  ' + name)
    add_file(entries, f'Library/dpkg/info/{package}.list', ('\n'.join(names) + '\n').encode())
    add_file(entries, f'Library/dpkg/info/{package}.md5sums', ('\n'.join(checksums) + '\n').encode())
    for name, entry in control.items():
        if name not in ('control', 'md5sums'):
            entries[f'Library/dpkg/info/{package}.{name}'] = entry
    return package
