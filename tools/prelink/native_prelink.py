#!/usr/bin/env python3
"""Plan ordinary ARM64 prelink pointer rebinding; never changes input images."""

import argparse
import base64
import hashlib
import itertools
import json
import struct
import xml.etree.ElementTree as ET
from bisect import bisect_right
from collections import Counter
from collections.abc import Iterable, Sequence
from pathlib import Path
from typing import TypedDict, Unpack

from tools.common.macho import backed_offset, exports, macho


def unserialize_xml(blob: bytes) -> object:
    return read_xml(ET.fromstring(blob.split(b'\0', 1)[0]), {})


def read_xml(element: ET.Element, refs: dict[str, object]) -> object:
    if 'IDREF' in element.attrib:
        return refs[element.attrib['IDREF']]
    if element.tag == 'dict':
        value = read_xml_dict(element, refs)
    elif element.tag == 'array':
        value = [read_xml(child, refs) for child in element]
    else:
        value = xml_scalar(element)
    if 'ID' in element.attrib:
        identifier = element.attrib['ID']
        if identifier in refs:
            raise ValueError('Duplicate XML object ID')
        refs[identifier] = value
    return value


def read_xml_dict(element: ET.Element, refs: dict[str, object]) -> dict[object, object]:
    children = list(element)
    if len(children) % 2:
        raise ValueError('Odd XML dictionary')
    value = {}
    for index in range(0, len(children), 2):
        key = read_xml(children[index], refs)
        if key in value:
            raise ValueError('Duplicate XML dictionary key')
        value[key] = read_xml(children[index + 1], refs)
    return value


def xml_scalar(element: ET.Element) -> object:
    tag = element.tag
    if tag in ('key', 'string'):
        return element.text or ''
    if tag == 'integer':
        return int(element.text or '0', 0)
    if tag == 'data':
        return base64.b64decode(''.join((element.text or '').split()), validate=True)
    if tag in ('true', 'false'):
        return tag == 'true'
    raise ValueError(f'Unsupported prelink XML element {tag}')


def unique_kernel_binding(
    address: int, original_exports: dict[int, list[str]], native_by_name: dict[str, set[int]]
) -> int:
    names = original_exports.get(address, [])
    choices = {new for name in names for new in native_by_name.get(name, set())}
    if len(choices) != 1:
        raise ValueError(f'Kernel target {address:#x} has {len(choices)} native address candidates: {names}')
    return next(iter(choices))


class OriginalOptions(TypedDict, total=False):
    inherited_original_sha256: str | None
    cuts: dict[tuple[str, str], tuple[int, str | None]] | None


def plan(
    native_data: bytes, original_data: bytes, roots: Iterable[str], slide: int, **original: Unpack[OriginalOptions]
) -> dict[str, object]:
    # cuts: {(component, declared dependency): (exact pointer count, native stub symbol or None)}. A cut dependency is
    # left out of the closure; each of its pointer sites in the component (exactly that many, or the plan fails) is
    # rebound to the native stub, an 8-byte zero constant, so the GOT load the component does reads NULL.
    # The original kernelcache's identity is inherited from the completed pass that made it: do not reread or hash it
    # merely to reproduce an existing digest.
    report, unbound = plan_original(original_data, roots, slide, **original)
    bind_native(report, unbound, native_data)
    return report


def plan_original(
    original_data: bytes,
    roots: Iterable[str],
    slide: int,
    *,
    inherited_original_sha256: str | None = None,
    cuts: dict[tuple[str, str], tuple[int, str | None]] | None = None,
) -> tuple[dict[str, object], dict[str, list[tuple]]]:
    """Run plan()'s part that reads only the original kernelcache.

    The closure, the extents and every pointer site, with the component pointers bound. The kernel and cut pointers'
    targets (new_target None here) and the checks against the native kernel are bind_native's, from `pending`:
    {'kernel': [(binding index, target, original export names)], 'cut': [(binding index, component, dependency, stub)]}.
    """
    check_inherited_identity(inherited_original_sha256)
    if slide % 0x4000:
        raise ValueError('Rebasing must keep the segments on whole VM pages (16 KiB)')
    original = macho(original_data)
    if original['kind'] != 2:
        raise ValueError('Expected MH_EXECUTE kernel images')
    parent = original['segments']
    metadata = prelink_info(original_data, parent)
    entries = prelink_entries(metadata)
    cuts = dict(cuts or {})
    for component, dependency in cuts:
        if dependency not in (entries.get(component, {}).get('OSBundleLibraries') or {}):
            raise ValueError(f'Cut {component} -> {dependency}: not a declared dependency')
    selected, interfaces = closure(entries, roots, cuts)
    owned, modules = component_segments(original_data, parent, entries)
    codeless = selected - modules.keys()
    check_codeless(entries, codeless)
    owners = Owners(owned)
    binder = PointerBinder(owners, selected, cuts, slide, exports(original_data, original))
    offsets = pointer_offsets(metadata['_PrelinkLinkKASLROffsets'])
    base = next(s['va'] for s in parent if s['name'] == '__PRELINK_TEXT')
    binder.bind_sites(original_data, parent, [base + offset for offset in offsets])
    bindings, cut_sites = binder.bindings, binder.cut_sites
    for edge, (expected, _) in cuts.items():
        if edge[0] in selected and cut_sites[edge] != expected:
            raise ValueError(
                f'Cut {edge[0]} -> {edge[1]}: {cut_sites[edge]} pointer sites, the allow-list says {expected}'
            )
    extents = rebased_extents(owners.owned, selected, slide)
    return {
        'native_sha256': None,
        'original_sha256': inherited_original_sha256,
        'slide': slide,
        'components': sorted(selected - codeless),
        'codeless_components': sorted(codeless),
        'kernel_interfaces': sorted(interfaces),
        'pointer_counts': dict(Counter(binding['kind'] for binding in bindings)),
        'extents': extents,
        'bindings': bindings,
        'images_modified': False,
        'cut_dependencies': [
            {'component': c, 'dependency': d, 'pointer_sites': cut_sites[(c, d)], 'stub': cuts[(c, d)][1]}
            for (c, d) in sorted(cuts)
            if c in selected
        ],
        'instruction_relocation_complete': False,
        'abi_qualified': False,
        'loadable': False,
    }, binder.unbound


def check_inherited_identity(sha256: str | None) -> None:
    if not isinstance(sha256, str) or len(sha256) != 64 or any(c not in '0123456789abcdef' for c in sha256):
        raise ValueError('A valid inherited original identity is required')


def prelink_info(data: bytes, parent: list[dict[str, object]]) -> dict[str, object]:
    info = next(s for s in parent if s['name'] == '__PRELINK_INFO')
    at = backed_offset(parent, info['va'], info['filesize'], len(data))
    return unserialize_xml(data[at : at + info['filesize']])


def prelink_entries(metadata: dict[str, object]) -> dict[str, dict[str, object]]:
    entries = {}
    for entry in metadata['_PrelinkInfoDictionary']:
        identifier = entry['CFBundleIdentifier']
        if identifier in entries:
            raise ValueError('Duplicate prelink identifier')
        entries[identifier] = entry
    return entries


def closure(
    entries: dict[str, dict[str, object]], roots: Iterable[str], cuts: dict[tuple[str, str], object]
) -> tuple[set[str], set[str]]:
    selected, interfaces = set(), set()
    pending = list(roots)
    while pending:
        identifier = pending.pop()
        if identifier in selected or identifier in interfaces:
            continue
        if identifier not in entries:
            raise ValueError(f'Missing declared dependency {identifier}')
        entry = entries[identifier]
        if entry.get('OSKernelResource') is True:
            interfaces.add(identifier)
            continue
        selected.add(identifier)
        pending.extend(d for d in (entry.get('OSBundleLibraries') or {}) if (identifier, d) not in cuts)
    return selected, interfaces


def component_segments(
    data: bytes, parent: list[dict[str, object]], entries: dict[str, dict[str, object]]
) -> tuple[list[tuple[int, int, str, dict[str, object]]], dict[str, dict[str, object]]]:
    owned, modules = [], {}
    for identifier, entry in entries.items():
        if entry.get('OSKernelResource') is True:
            continue
        va = entry.get('_PrelinkExecutableLoadAddr')
        if va is None or not entry.get('_PrelinkExecutableSize'):
            continue
        header = backed_offset(parent, va, 32, len(data))
        module = macho(data, header)
        if module['kind'] != 11:
            raise ValueError('Expected MH_KEXT_BUNDLE component')
        modules[identifier] = module
        owned.extend(
            (segment['va'], segment['va'] + segment['size'], identifier, segment)
            for segment in module['segments']
            if segment['size'] and segment['name'] != '__LINKEDIT'
        )
    return owned, modules


def check_codeless(entries: dict[str, dict[str, object]], codeless: set[str]) -> None:
    executable_keys = (
        'CFBundleExecutable',
        '_PrelinkExecutableLoadAddr',
        '_PrelinkExecutableSourceAddr',
        '_PrelinkExecutableSize',
        '_PrelinkKmodInfo',
    )
    for identifier in codeless:
        entry = entries[identifier]
        if (
            any(key in entry for key in executable_keys)
            or not isinstance(entry.get('IOKitPersonalities'), dict)
            or not entry['IOKitPersonalities']
        ):
            raise ValueError('Codeless component must contain personalities and no executable fields: ' + identifier)


def pointer_offsets(packed: bytes) -> tuple[int, ...]:
    (count,) = struct.unpack_from('<I', packed)
    if len(packed) != 4 + count * 4:
        raise ValueError('Invalid pointer-table extent')
    offsets = struct.unpack_from(f'<{count}I', packed, 4)
    if len(set(offsets)) != count:
        raise ValueError('Duplicate pointer relocation site')
    return offsets


def rebased_extents(
    owned: list[tuple[int, int, str, dict[str, object]]], selected: set[str], slide: int
) -> list[dict[str, object]]:
    extents = []
    for start, end, identifier, segment in owned:
        if identifier not in selected:
            continue
        if not 0 <= start + slide < end + slide <= 1 << 64:
            raise ValueError('Rebased segment outside 64-bit address space')
        extents.append({'module': identifier, 'segment': segment['name'], 'va': start + slide, 'size': end - start})
    return extents


class Owners:
    def __init__(self, owned: list[tuple[int, int, str, dict[str, object]]]):
        self.owned = sorted(owned, key=lambda item: item[:3])
        for left, right in itertools.pairwise(self.owned):
            if left[1] > right[0]:
                raise ValueError(f'Overlapping component extents: {left[2]} and {right[2]}')
        self.starts = [item[0] for item in self.owned]

    def owner(self, address: int) -> tuple[int, int, str, dict[str, object]] | None:
        index = bisect_right(self.starts, address) - 1
        return self.owned[index] if index >= 0 and address < self.owned[index][1] else None


class PointerBinder:
    def __init__(
        self,
        owners: Owners,
        selected: set[str],
        cuts: dict[tuple[str, str], tuple[int, str | None]],
        slide: int,
        symbols: dict[int, list[str]],
    ):
        self.owners = owners
        self.selected = selected
        self.cuts = cuts
        self.slide = slide
        self.symbols = symbols
        self.bindings = []
        self.unbound = {'kernel': [], 'cut': []}
        self.cut_sites = dict.fromkeys(cuts, 0)

    def bind_sites(self, data: bytes, parent: list[dict[str, object]], sites: Iterable[int]) -> None:
        for site in sites:
            source = self.owners.owner(site)
            if source is None or source[2] not in self.selected:
                continue
            if source != self.owners.owner(site + 7):
                raise ValueError('Pointer spans component extent')
            (target,) = struct.unpack_from('<Q', data, backed_offset(parent, site, 8, len(data)))
            self.bind(site, target, source[2])

    def bind(self, site: int, target: int, module: str) -> None:
        destination = self.owners.owner(target)
        replacement = None
        if destination and (module, destination[2]) in self.cuts and destination[2] not in self.selected:
            kind = 'cut'
            stub = self.cuts[(module, destination[2])][1]
            self.unbound['cut'].append((len(self.bindings), module, destination[2], stub))
            self.cut_sites[(module, destination[2])] += 1
        elif destination:
            if destination[2] not in self.selected:
                raise ValueError(f'Undeclared pointer dependency {destination[2]} from {module}')
            replacement = target + self.slide
            kind = 'component'
        else:
            kind = 'kernel'
            self.unbound['kernel'].append((len(self.bindings), target, self.symbols.get(target, [])))
        # A native export (the kernel and cut targets) is a 64-bit symbol value: always in range.
        if not 0 <= site + self.slide < 1 << 64 or not 0 <= (target if replacement is None else replacement) < 1 << 64:
            raise ValueError('Rebased pointer outside 64-bit address space')
        self.bindings.append(
            {
                'site': site,
                'target': target,
                'new_site': site + self.slide,
                'new_target': replacement,
                'kind': kind,
                'module': module,
            }
        )


def native_bindings(
    native_data: bytes,
    native: dict[str, object],
    kernel_targets: Iterable[tuple[int, list[str]]],
    cut_stubs: Iterable[tuple[str, str, str | None]],
    extents: Iterable[Sequence[int]],
) -> tuple[list[int], list[int], dict[int, list[str]]]:
    """Run the native kernel's half of plan().

    Each kernel target's native address (kernel_targets: [(target, original export names)]), each cut's stub address
    (cut_stubs: [(component, dependency, stub)]), and the checks that the rebased extents ([(va, size)]) miss the native
    segments. The exports, for the caller's own markers.
    """
    native_symbols = {}
    exported = exports(native_data, native)
    for address, names in exported.items():
        for name in names:
            native_symbols.setdefault(name, set()).add(address)
    addresses = []
    for target, names in kernel_targets:
        replacement = unique_kernel_binding(target, {target: names}, native_symbols)
        if not any(s['va'] <= replacement < s['va'] + s['size'] for s in native['segments']):
            raise ValueError('Native export is outside mapped kernel segments')
        addresses.append(replacement)
    stubs = []
    for component, dependency, stub in cut_stubs:
        if not stub or len(native_symbols.get(stub, ())) != 1:
            raise ValueError(f'Cut {component} -> {dependency} needs one native stub {stub}')
        stubs.append(next(iter(native_symbols[stub])))
    for va, size in extents:
        for s in native['segments']:
            if s['size'] and va < s['va'] + s['size'] and s['va'] < va + size:
                raise ValueError(f'Rebased component collides with native {s["name"]}')
    return addresses, stubs, exported


def bind_native(report: dict[str, object], unbound: dict[str, list[tuple]], native_data: bytes) -> dict[int, list[str]]:
    """Run plan()'s native half on plan_original's report.

    The kernel and cut pointers' new targets, the native checks, native_sha256. Returns the exports of the native
    kernel.
    """
    native = macho(native_data)
    if native['kind'] != 2:
        raise ValueError('Expected MH_EXECUTE kernel images')
    bindings = report['bindings']
    addresses, stubs, exported = native_bindings(
        native_data,
        native,
        [(target, names) for _, target, names in unbound['kernel']],
        [(component, dependency, stub) for _, component, dependency, stub in unbound['cut']],
        [(e['va'], e['size']) for e in report['extents']],
    )
    for (index, _, _), address in zip(unbound['kernel'], addresses, strict=False):
        bindings[index]['new_target'] = address
    for (index, _, _, _), address in zip(unbound['cut'], stubs, strict=False):
        bindings[index]['new_target'] = address
    report['native_sha256'] = hashlib.sha256(native_data).hexdigest()
    return exported


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--prelinked', required=True, type=Path)
    parser.add_argument('--root', required=True, action='append')
    parser.add_argument('--slide', required=True, type=lambda value: int(value, 0))
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    report = plan(args.kernel.read_bytes(), args.prelinked.read_bytes(), args.root, args.slide)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(
        json.dumps(
            {key: report[key] for key in ('components', 'kernel_interfaces', 'pointer_counts', 'loadable')}, indent=2
        )
    )


if __name__ == '__main__':
    main()
