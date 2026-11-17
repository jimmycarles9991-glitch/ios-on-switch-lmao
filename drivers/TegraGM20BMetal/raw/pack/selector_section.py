"""The pack's prepared selector programs, indexed by the selected build's normalized config IDs."""

import struct
from collections.abc import Callable
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    import directlink

TABLE = struct.Struct('<II')
CONFIG = struct.Struct('<II')
INDEX = struct.Struct('<II')


def read(reader: 'directlink.Reader', program: Callable) -> dict | None:
    count = reader.u32()
    if not count:
        return None
    assume = reader.u32()
    return {'assume': assume, 'configs': [(reader.u32(), program(reader)) for _ in range(count)]}


def programs(record: dict) -> list[dict]:
    return [program for _, program in record['selectors']['configs']] if record.get('selectors') else []


def of_record(record: dict, index_of: Callable) -> dict | None:
    selectors = record.get('selectors')
    if not selectors:
        return None
    return {'assume': selectors['assume'], 'configs': [(config, index_of(p)) for config, p in selectors['configs']]}


def table_bytes(selectors: dict) -> bytes:
    configs = selectors['configs']
    return TABLE.pack(len(configs), selectors['assume']) + b''.join(CONFIG.pack(*config) for config in configs)


def section_bytes(pipelines: list[dict]) -> bytes:
    tables = [(i, table_bytes(p['selectors'])) for i, p in enumerate(pipelines) if p.get('selectors')]
    if not tables:
        return b''
    head = 8 + INDEX.size * len(tables)
    index, body = bytearray(), bytearray()
    for record, table in tables:
        index += INDEX.pack(record, head + len(body))
        body += table
    return struct.pack('<II', len(tables), 0) + bytes(index) + bytes(body)


def expected(text: str) -> dict[int, set[int]]:
    rows = [
        line.split() for line in text.splitlines() if line.strip() and not line.lstrip().startswith(('#', 'layout '))
    ]
    ids = {value: i for i, value in enumerate(sorted({bytes.fromhex(row[1]) for row in rows}), 1)}
    states: dict[int, set[int]] = {}
    for key, value in rows:
        states.setdefault(int(key, 16), set()).add(ids[bytes.fromhex(value)])
    return states


def coverage(records: list[dict], text: str, key_of: Callable) -> list[str]:
    wanted = expected(text)
    covered, lines = set(), []
    for record in records:
        key = key_of(record)
        if key not in wanted:
            continue
        if record.get('outcome', 0):
            raise ValueError(f'selector state {key:016x} refused: {record["why"]}')
        selected = (
            [config for config, _ in record.get('selectors', {}).get('configs', [])] if record.get('selectors') else []
        )
        generic = record.get('selector_generic', [])
        actual = selected + generic
        if len(actual) != len(set(actual)) or set(actual) != wanted[key]:
            raise ValueError(
                f'selector state {key:016x}: expected IDs {sorted(wanted[key])}, resolved {sorted(actual)}'
            )
        for config in actual:
            covered.add((key, config))
            reason = (
                'byte-identical generic (code, data, bindings and ordering)'
                if config in generic
                else 'compatible specialization'
            )
            lines.append(
                f'  selector {key:016x} config {config} {record.get("form", "default") or "default"}: {reason}'
            )
    missing = {(key, config) for key, values in wanted.items() for config in values} - covered
    if missing:
        raise ValueError(
            'selector observations absent from pack: '
            + ', '.join(f'{key:016x}/{config}' for key, config in sorted(missing))
        )
    ids = set().union(*wanted.values()) if wanted else set()
    return [
        f'selector coverage: {len(covered)} observations, {len(ids)} normalized IDs, {len(lines)} form/config proofs',
        *lines,
    ]
