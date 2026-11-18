import argparse
import base64
import binascii
import hashlib
import hmac
import json
import shutil
import sqlite3
import tempfile
from pathlib import Path


def parse(bookmark: str) -> tuple[bytes, bytes]:
    if not bookmark.startswith('fp:/'):
        raise ValueError('not a FileProvider bookmark')
    encoded, separator, item = bookmark[4:].partition('/')
    parts = item.split('/', 2)
    if not separator or len(parts) != 3 or not parts[0] or not parts[2]:
        raise ValueError('bookmark has no provider, domain, and item identifier')
    try:
        signature = base64.b64decode(encoded.replace('_', '/'), validate=True)
    except (ValueError, binascii.Error) as error:
        raise ValueError('invalid bookmark authentication code') from error
    if len(signature) != hashlib.sha256().digest_size:
        raise ValueError('bookmark authentication code is not SHA-256')
    return signature, item.encode()


def hmac_matches(bookmark: str, key: bytes, consumer: str, session: bytes | None) -> bool:
    signature, item = parse(bookmark)
    authenticated = item + consumer.encode() + (b'legacy' if session is None else session)
    return hmac.compare_digest(signature, hmac.digest(key, authenticated, 'sha256'))


def inspect(database: Path, bookmark: str, sessions: dict[str, bytes | None]) -> list[dict]:
    parse(bookmark)
    with tempfile.TemporaryDirectory(prefix='switch-fileprovider-') as temporary:
        snapshot = Path(temporary) / 'AccessControl.db'
        shutil.copyfile(database, snapshot)
        for suffix in ['-wal', '-shm']:
            source = Path(str(database) + suffix)
            if source.is_file():
                shutil.copyfile(source, Path(str(snapshot) + suffix))
        connection = sqlite3.connect(snapshot.as_uri() + '?mode=ro', uri=True)
        try:
            return [
                {
                    'consumer': consumer,
                    'matching_hmac_sessions': [
                        label for label, session in sessions.items() if hmac_matches(bookmark, key, consumer, session)
                    ],
                }
                for consumer, key in connection.execute('SELECT identifier, symmetric_key FROM bundle_keys')
            ]
        finally:
            connection.close()


def main() -> None:
    parser = argparse.ArgumentParser(
        description='Read-only comparison of bookmark HMACs with saved consumer keys and installation sessions.'
    )
    parser.add_argument('database', type=Path)
    parser.add_argument('bookmark')
    parser.add_argument('--session', action='append', default=[], metavar='LABEL=HEX')
    args = parser.parse_args()
    sessions: dict[str, bytes | None] = {'legacy': None}
    for value in args.session:
        label, separator, encoded = value.partition('=')
        if not separator or not label or label in sessions:
            parser.error('--session must be a unique LABEL=HEX')
        try:
            sessions[label] = bytes.fromhex(encoded)
        except ValueError:
            parser.error('--session must contain hexadecimal bytes')
    try:
        result = inspect(args.database, args.bookmark, sessions)
    except (ValueError, OSError, sqlite3.Error) as error:
        parser.exit(1, f'{error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
