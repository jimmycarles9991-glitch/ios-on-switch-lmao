#!/usr/bin/env python3
"""Construct an experimental native prelink candidate; no runtime qualification.

The work splits by what it reads (kernel/rules/kernel.bzl kernel_prelink runs each part as its own action):

    emit_prelink.py plan ORIGINAL-ARGS --plan-output PLAN       the original kernelcache only: the closure, every
                                                                pointer site, the coalesced segments with every
                                                                original-only byte written, the prelink info XML
    emit_prelink.py apply --plan PLAN --kernel K --output O     the native kernel: its exports bound, the segments
                                                                appended, the KCID, the header (a driver edit reruns
                                                                only this)
    emit_prelink.py report ORIGINAL-ARGS --kernel K --expect O --report-output R
                                                                both in one process, with the report; refuses unless
                                                                its image is byte for byte O
    emit_prelink.py ORIGINAL-ARGS --kernel K --output O         both in one process: O and O's .json (the old form)

ORIGINAL-ARGS: --prelinked, --root, --new-root, --cut, --defer, --slide, --early-security-init,
--inherit-original-report, --previous. emit() = apply(prepare()): the same bytes, whichever way it runs.
--options PRELINK_JSON stands for --root through --early-security-init: the prelink.json //install:kernel_inputs puts in
the release, read the way kernel_prelink passes its flags.
"""

import argparse
import array
import base64
import copy
import dataclasses
import hashlib
import itertools
import json
import struct
import sys
import xml.etree.ElementTree as ET
from bisect import bisect_left
from pathlib import Path
from typing import NamedTuple, Self

sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_prelink import backed_offset, macho, native_bindings, plan_original, prelink_info

# Standard SHA-256 round constants, stored as 64 little-endian words. Some
# assemblers embed these data in executable sections; do not decode as code.
SHA256_K_DIGEST = '74ef7306e7452d6859b6463ce496b8df30925f69e1b2969e1f3f34bbc9c6af04'
VECTOR_REPEAT_DIGEST = 'f7bb30778915b574b63a17573babfdeb97251ae92fb57e7302e69783f454f92a'
ADLER32_VECTOR_DIGEST = '48bdf8e57f8647f2ef3cfda5b500209c4be9b5742632db0ff3614aca8d7ee63d'
MATH_LITERAL_POOLS = (
    ('00000000004a87c0f039fafe422e8640', 2160, '1a5eb119b57d453a6123eb839b259b96f2ff4f687fc8aabfdcf965ded106df81'),
    ('0000807f0000f042db0f493f00008039', 1248, '92c7b2f87685a1e7a92cad6e7d408fdbc03bd67d1207b6579214040f413b2161'),
    ('1f2003d500000000001018c00ee52615', 2188, 'c103ce1f2e204bbbfcc2069de79758b0ea399e5e7b39b2b8337cbf3d3f7f960a'),
    ('0000c1c00000807fff799f501344d33f', 1088, '22b9e038ad1cbdef1265a71f9213354287637a561eaa52f9ec25f90d10bd0a7c'),
    ('0000c1c00000807f0000000084aec68f', 212, '43ef8598a507444b21d8a2abbe4065554baaa3b6a29dbe390465cc0ba0dfbe28'),
    ('9b1a86a049faa8bd053f4e7b9dee213e', 56, 'e874aeba59eebdebdf68d18b1196d1ca6aeee12b110985a856f8fe404e0c76ee'),
)
# Original kexts prelinked beyond the V20 roots, the dependencies left out of the prelink and the personalities kept
# out of the boot catalogue: declared by the drivers that need them (prelink, prelink_cuts, prelink_deferred in
# the drivers' BUILD.bazel files, which say why for each) and passed here by the kernel_prelink rule.
NEW_ORIGINAL_COMPONENTS = set()  # --new-root BUNDLE
# Declared dependencies left out of the prelink, by exact pointer count (plan() fails the build when the original
# kernelcache has a different number of sites): (component, dependency) -> (pointer sites, native stub). A nonzero cut
# rebinds each site, a GOT entry of the dependency's metaclass, to the stub (8 zero bytes), so that OSDynamicCast yields
# NULL. The emitted prelink info drops the declaration.
CUT_STUB = '_switch_prelink_cut_null'
CUT_DEPENDENCIES = {}  # --cut COMPONENT=DEPENDENCY=SITES (the stub when SITES > 0)
DEFERRED_PERSONALITIES = {}  # --defer COMPONENT=PERSONALITY
# main() requires the --root list to be the V20 roots followed by sorted(NEW_ORIGINAL_COMPONENTS).

COALESCED = {
    '__PRELINK_TEXT': '__TEXT',
    '__PLK_TEXT_EXEC': '__TEXT_EXEC',
    '__PLK_DATA_CONST': '__DATA_CONST',
    '__PRELINK_DATA': '__DATA',
}


# XNU's VM page: the kernelcache's segments are mapped and freed as whole VM pages (16 KiB, as on every iPad).
VM_PAGE = 0x4000


def align(value: int, page: int = 4096) -> int:
    return (value + page - 1) & ~(page - 1)


def to_xml(value: object) -> ET.Element:
    if isinstance(value, dict):
        node = ET.Element('dict')
        for key, child in value.items():
            ET.SubElement(node, 'key').text = key
            node.append(to_xml(child))
    elif isinstance(value, list):
        node = ET.Element('array')
        node.extend(to_xml(child) for child in value)
    elif isinstance(value, bool):
        node = ET.Element('true' if value else 'false')
    elif isinstance(value, int):
        if not 0 <= value < 1 << 64:
            raise ValueError('Metadata integer outside 64-bit range')
        node = ET.Element('integer', {'size': '64'})
        node.text = hex(value)
    elif isinstance(value, bytes):
        node = ET.Element('data')
        node.text = base64.b64encode(value).decode()
    elif isinstance(value, str):
        node = ET.Element('string')
        node.text = value
    else:
        raise ValueError('Unsupported metadata value')
    return node


def pc_relative_target(word: int, pc: int) -> tuple[int, bool] | None:
    def signed(value: int, bits: int) -> int:
        return value - (1 << bits) if value & (1 << (bits - 1)) else value

    if word & 0x9F000000 in (0x10000000, 0x90000000):
        immediate = signed(((word >> 5) & 0x7FFFF) << 2 | ((word >> 29) & 3), 21)
        page = bool(word & 0x80000000)
        return ((pc & ~4095) + (immediate << 12), True) if page else (pc + immediate, False)
    if word & 0x7C000000 == 0x14000000:
        return pc + (signed(word & 0x3FFFFFF, 26) << 2), False
    if word & 0xFF000010 == 0x54000000 or word & 0x7E000000 == 0x34000000 or word & 0x3B000000 == 0x18000000:
        return pc + (signed((word >> 5) & 0x7FFFF, 19) << 2), False
    if word & 0x7E000000 == 0x36000000:
        return pc + (signed((word >> 5) & 0x3FFF, 14) << 2), False
    return None


def instruction_inventory(
    original_data: bytes, parent: list[dict[str, object]], modules: dict[str, dict[str, object]]
) -> dict[str, object]:
    extents = [s for module in modules.values() for s in module['segments'] if s['size'] and s['name'] != '__LINKEDIT']
    literal_ranges, instruction_count = [], 0
    for identifier, module in modules.items():
        for segment in module['segments']:
            if segment['name'] != '__TEXT_EXEC' or not segment['filesize']:
                continue
            at = backed_offset(parent, segment['va'], segment['filesize'], len(original_data))
            blob = original_data[at : at + segment['filesize']]
            literals = known_inline_literals(blob)
            literal_ranges.extend(
                {'module': identifier, 'va': segment['va'] + start, 'size': size, 'sha256': digest}
                for start, size, digest in literals
            )
            spans = [(start, start + size) for start, size, _ in literals]
            instruction_count += internal_pc_relative_count(blob, segment['va'], spans, extents)
    return {
        'pc_relative_encodings_preserved': instruction_count,
        'known_inline_data': literal_ranges,
        'register_dataflow_qualified': False,
    }


def known_inline_literals(blob: bytes) -> list[tuple[int, int, str]]:
    literals = []
    # Complete identity is required: a prefix alone never excludes code.
    for prefix, size, digest, prefix_offset in (
        (bytes.fromhex('982f8a42'), 256, SHA256_K_DIGEST, 0),
        (bytes.fromhex('0001000100010001'), 528, VECTOR_REPEAT_DIGEST, 64),
        (bytes(range(32, 24, -1)), 40, ADLER32_VECTOR_DIGEST, 0),
        *((bytes.fromhex(prefix), size, digest, 0) for prefix, size, digest in MATH_LITERAL_POOLS),
    ):
        search = 0
        while (search := blob.find(prefix, search)) >= 0:
            start = search - prefix_offset
            if start >= 0 and start % 4 == 0 and hashlib.sha256(blob[start : start + size]).hexdigest() == digest:
                literals.append((start, size, digest))
            search += 4
    return literals


def internal_pc_relative_count(
    blob: bytes, va: int, literals: list[tuple[int, int]], extents: list[dict[str, object]]
) -> int:
    count = 0
    for offset in range(0, len(blob) - 3, 4):
        if any(start <= offset < end for start, end in literals):
            continue
        (word,) = struct.unpack_from('<I', blob, offset)
        decoded = pc_relative_target(word, va + offset)
        if decoded is None:
            continue
        target, page = decoded
        if page:
            internal = any((s['va'] & ~4095) <= target < align(s['va'] + s['size']) for s in extents)
        else:
            internal = any(s['va'] <= target < s['va'] + s['size'] for s in extents)
        if not internal:
            raise ValueError(f'External PC-relative encoding at {va + offset:#x}; no instruction rewrite supported')
        count += 1
    return count


# The prelink info's KCID is the one native-dependent value in its XML: prepare renders the XML with this stand-in
# (16 bytes, so the same 24 base64 characters long) and apply puts the real one in its place.
KCID_STAND_IN = b'\xa5\x5a' * 8
KCID_DOMAIN = b'SWITCH-MEMORY-V21-KCID\0'
PLAN_MAGIC = b'SWCHPLK1'


class Plan:
    """What apply needs from the original kernelcache.

    The coalesced segments back to back (every byte that does not depend on the native kernel written), where the
    native kernel's pointers go, and the prelink info XML around its KCID.
    """

    def __init__(self, **fields: object):
        self.__dict__.update(fields)

    def save(self, path: Path) -> None:
        """One file: magic, u64 header length, the header (JSON), then the blobs at the header's offsets."""
        blobs, layout = [], {}
        for name in ('blob', 'kernel_sites', 'kernel_groups', 'xml_head', 'xml_tail'):
            data = bytes(getattr(self, name))
            layout[name] = (sum(len(b) for b in blobs), len(data))
            blobs.append(data)
        header = json.dumps(
            {
                'slide': self.slide,
                'original_sha256': self.original_sha256,
                'segments': self.segments,
                'targets': self.targets,
                'cuts': self.cuts,
                'extents': self.extents,
                'early_security_init': self.early_security_init,
                'index': self.index,
                'layout': layout,
            },
            sort_keys=True,
            separators=(',', ':'),
        ).encode()
        path.write_bytes(PLAN_MAGIC + struct.pack('<Q', len(header)) + header + b''.join(blobs))

    @classmethod
    def load(cls, path: Path) -> Self:
        data = path.read_bytes()
        if data[:8] != PLAN_MAGIC:
            raise ValueError('Not a prelink plan')
        (length,) = struct.unpack_from('<Q', data, 8)
        header = json.loads(data[16 : 16 + length])
        base, fields = 16 + length, {}
        layout = header.pop('layout')
        if len(data) != base + sum(size for _, size in layout.values()):
            raise ValueError('Prelink plan has trailing bytes')
        for name, (at, size) in layout.items():
            fields[name] = data[base + at : base + at + size]
        for name, kind in (('kernel_sites', 'Q'), ('kernel_groups', 'I')):
            values = array.array(kind)
            values.frombytes(fields[name])
            fields[name] = values
        return cls(**header, **fields)


class Original(NamedTuple):
    data: bytes
    roots: list[str]
    slide: int
    early_security_init: dict[str, int] | None
    inherited_report: dict[str, object]


@dataclasses.dataclass
class Kernelcache:
    data: bytes
    parent: list[dict[str, object]]
    entries: dict[str, dict[str, object]]
    modules: dict[str, dict[str, object]]


class NativePointers(NamedTuple):
    targets: list[list[object]]
    kernel_sites: array.array
    kernel_groups: array.array
    cut_sites: list[list[object]]


# The coalesced segments back to back, as apply appends them to the native kernel (each a 4 KiB multiple, so
# contiguous there too): an offset in `output` here is one in the candidate less the first segment's file offset.
class CoalescedImage:
    def __init__(self) -> None:
        self.output = bytearray()
        self.new_segments = []
        self.writes = []  # (offset, length) of every original-only write after the copies, in order

    def append_segment(
        self, name: str, va: int, blob: bytes | bytearray, size: int, protection: int
    ) -> tuple[dict[str, object], int]:
        if va % VM_PAGE or size % VM_PAGE or len(blob) > size:
            raise ValueError('Coalesced segment must be whole VM pages')
        if any(s['size'] and va < s['va'] + s['size'] and s['va'] < va + size for s in self.new_segments):
            raise ValueError('Coalesced segment collides with existing mapping')
        fileoff = len(self.output)
        self.output.extend(blob)
        self.output.extend(bytes(size - len(blob)))
        result = {'name': name, 'va': va, 'size': size, 'filesize': size, 'fileoff': fileoff}
        self.new_segments.append(result)
        return result, protection

    def new_offset(self, address: int, length: int) -> int:
        return backed_offset(self.new_segments, address, length, len(self.output))

    def write(self, fmt: str, at: int, *values: int) -> None:
        struct.pack_into(fmt, self.output, at, *values)
        self.writes.append((at, struct.calcsize(fmt)))


class NativeSites:
    def __init__(self, pointers: NativePointers):
        self.sites = sorted([*pointers.kernel_sites, *(at for at, *_ in pointers.cut_sites)])
        if any(b - a < 8 for a, b in itertools.pairwise(self.sites)):
            raise ValueError('Native kernel pointer sites overlap')

    def hits(self, at: int, length: int) -> bool:
        i = bisect_left(self.sites, at - 7)
        return i < len(self.sites) and self.sites[i] < at + length


def prepare(original: Original) -> tuple[Plan, dict[str, object], dict[str, list[tuple]]]:
    """Run emit()'s part that reads only the original kernelcache.

    Returns (the plan, the report without its native fields, the report's unbound pointers).
    """
    inherited = original.inherited_report
    slide = original.slide
    report, unbound = plan_original(
        original.data,
        original.roots,
        slide,
        cuts=CUT_DEPENDENCIES,
        inherited_original_sha256=inherited['original_sha256'],
    )
    check_inherited_contract(report, inherited)
    cache = read_kernelcache(original.data, report['components'])
    add_original_findings(report, inherited, cache)
    early_security_init = original.early_security_init or {}
    report['early_security_initializers'] = early_security_initializers(cache, early_security_init, slide)
    image = CoalescedImage()
    segments = copy_coalesced_segments(image, cache, report['kernel_interfaces'], slide)
    pointers = component_pointers(image, report['bindings'], unbound)
    rebase_headers(image, cache, slide)
    report['module_entrypoints'] = module_entrypoints(image, cache, report['components'], slide)
    report['sparse_index'], index_at = sparse_index(image, cache, report, slide)
    # Applying the native pointers last gives emit's bytes only when no original-only write after them overlaps one
    # (none does: each pointer site is its own 8 bytes) and none is in code (checked below with the copied code).
    native_sites = NativeSites(pointers)
    if any(native_sites.hits(at, length) for at, length in image.writes):
        raise ValueError('An original-only write overlaps a native kernel pointer site')
    xml_head, xml_tail = prelink_info_xml(
        selected_entries(cache, report, early_security_init, slide), kaslr_offsets(image, report)
    )
    text_hashes = executable_hashes(image, cache, native_sites, inherited, slide)
    report['kcid_derivation'] = 'SHA256(domain + new_native_bytes + inherited_original_sha256_bytes)[:16]'
    report.update(
        candidate_sha256=None,
        coalesced_segments=None,
        original_executable_sha256=text_hashes,
        candidate_bytes=None,
        source_inputs_modified=False,
        runtime_qualified=False,
        loadable=False,
    )
    result = Plan(
        slide=slide,
        original_sha256=inherited['original_sha256'],
        segments=[[s['name'], s['va'], s['size'], protection, s['fileoff']] for s, protection in segments],
        blob=image.output,
        targets=pointers.targets,
        kernel_sites=pointers.kernel_sites,
        kernel_groups=pointers.kernel_groups,
        cuts=pointers.cut_sites,
        extents=[[e['va'], e['size']] for e in report['extents']],
        early_security_init=bool(early_security_init),
        index=[report['sparse_index']['va'], report['sparse_index']['size'], index_at],
        xml_head=xml_head,
        xml_tail=xml_tail,
    )
    return result, report, unbound


def check_inherited_contract(report: dict[str, object], inherited: dict[str, object]) -> None:
    # Required relocation work still parses original bytes. Reuse only findings
    # wholly determined by the unchanged original modules and fixed slide.
    for key in ('slide', 'kernel_interfaces'):
        if report[key] != inherited[key]:
            raise ValueError('Inherited original contract changed: ' + key)
    new_codeless = NEW_ORIGINAL_COMPONENTS & set(report['codeless_components'])
    if set(report['codeless_components']) != set(inherited['codeless_components']) | new_codeless:
        raise ValueError('Inherited original contract changed: codeless_components')
    if set(report['components']) != set(inherited['components']) | (NEW_ORIGINAL_COMPONENTS - new_codeless):
        raise ValueError('Original component closure differs from V20')
    old_extents = [e for e in report['extents'] if e['module'] not in NEW_ORIGINAL_COMPONENTS]
    if old_extents != inherited['extents']:
        raise ValueError('An inherited original component extent changed')


def read_kernelcache(data: bytes, components: list[str]) -> Kernelcache:
    parent = macho(data)['segments']
    metadata = prelink_info(data, parent)
    entries = {e['CFBundleIdentifier']: e for e in metadata['_PrelinkInfoDictionary']}
    modules = {}
    for identifier in components:
        va = entries[identifier]['_PrelinkExecutableLoadAddr']
        modules[identifier] = macho(data, backed_offset(parent, va, 32, len(data)))
    return Kernelcache(data, parent, entries, modules)


def add_original_findings(report: dict[str, object], inherited: dict[str, object], cache: Kernelcache) -> None:
    added_inventory = instruction_inventory(
        cache.data, cache.parent, {k: v for k, v in cache.modules.items() if k in NEW_ORIGINAL_COMPONENTS}
    )
    report['pc_relative_encodings_preserved'] = (
        inherited['pc_relative_encodings_preserved'] + added_inventory['pc_relative_encodings_preserved']
    )
    report['known_inline_data'] = copy.deepcopy(inherited['known_inline_data']) + added_inventory['known_inline_data']
    report['register_dataflow_qualified'] = False
    report['new_component_instruction_inventory'] = added_inventory
    report['new_original_components'] = sorted(NEW_ORIGINAL_COMPONENTS)
    report['original_input_validation'] = {
        'kind': 'all_33_original_modules_inherited_from_v20',
        'whole_original_hash_recomputed': False,
        'old_original_executable_hashes_recomputed': False,
        'new_original_executable_hashes_computed': sorted(NEW_ORIGINAL_COMPONENTS),
        'old_instruction_inventory_recomputed': False,
        'actual_copied_executable_bytes_compared': True,
        'original_bytes_consumed_for_required_relink': True,
    }


def early_security_initializers(cache: Kernelcache, early_security_init: dict[str, int], slide: int) -> dict[str, int]:
    initializers = {}
    for identifier, address in early_security_init.items():
        if identifier not in cache.modules or cache.entries[identifier].get('AppleSecurityExtension') is not True:
            raise ValueError('Early initializer requires a selected security extension')
        if address % 4 or not any(
            s['name'] == '__TEXT_EXEC'
            and s['initprot'] == 5
            and not s['maxprot'] & 2
            and s['va'] <= address
            and address + 4 <= s['va'] + s['filesize']
            for s in cache.modules[identifier]['segments']
        ):
            raise ValueError('Early security initializer outside original module code')
        if not 0 <= address + slide < 1 << 64:
            raise ValueError('Early initializer address overflow')
        initializers[identifier] = address + slide
    return initializers


def copy_coalesced_segments(
    image: CoalescedImage, cache: Kernelcache, kernel_interfaces: list[str], slide: int
) -> list[tuple[dict[str, object], int]]:
    # Keep coalesced geometry, but only copy selected component segments.
    # Packed kernel-resource records are copied as original readonly metadata.
    segments = []
    for coalesced_name, component_name in COALESCED.items():
        source = next(s for s in cache.parent if s['name'] == coalesced_name)
        blob = bytearray(source['size'])
        copy_component_segments(blob, source, cache, component_name)
        if coalesced_name == '__PRELINK_TEXT':
            copy_kernel_interfaces(blob, source, cache, kernel_interfaces)
        segments.append(
            image.append_segment(coalesced_name, source['va'] + slide, blob, source['size'], source['initprot'])
        )
    return segments


def copy_component_segments(
    blob: bytearray, source: dict[str, object], cache: Kernelcache, component_name: str
) -> None:
    for module in cache.modules.values():
        for segment in module['segments']:
            if segment['name'] != component_name or not segment['filesize']:
                continue
            begin = segment['va'] - source['va']
            if begin < 0 or begin + segment['size'] > len(blob):
                raise ValueError('Component is outside original coalesced range')
            fileoff = backed_offset(cache.parent, segment['va'], segment['filesize'], len(cache.data))
            blob[begin : begin + segment['filesize']] = cache.data[fileoff : fileoff + segment['filesize']]


def copy_kernel_interfaces(
    blob: bytearray, source: dict[str, object], cache: Kernelcache, kernel_interfaces: list[str]
) -> None:
    for identifier in kernel_interfaces:
        entry = cache.entries[identifier]
        va, length = entry['_PrelinkExecutableLoadAddr'], entry['_PrelinkExecutableSize']
        begin = va - source['va']
        at = backed_offset(cache.parent, va, length, len(cache.data))
        if begin < 0 or begin + length > len(blob):
            raise ValueError('Kernel interface outside readonly prelink range')
        blob[begin : begin + length] = cache.data[at : at + length]


def component_pointers(
    image: CoalescedImage, bindings: list[dict[str, object]], unbound: dict[str, list[tuple]]
) -> NativePointers:
    # The component pointers here; the kernel and cut pointers' sites for apply, which binds them to the native kernel.
    kernel = {index: (target, names) for index, target, names in unbound['kernel']}
    cut = {index: (component, dependency, stub) for index, component, dependency, stub in unbound['cut']}
    targets, groups, kernel_sites, kernel_groups, cut_sites = [], {}, array.array('Q'), array.array('I'), []
    for index, binding in enumerate(bindings):
        at = image.new_offset(binding['new_site'], 8)
        if index in kernel:
            target, names = kernel[index]
            if target not in groups:
                groups[target] = len(targets)
                targets.append([target, names])
            kernel_sites.append(at)
            kernel_groups.append(groups[target])
        elif index in cut:
            cut_sites.append([at, *cut[index]])
        else:
            image.write('<Q', at, binding['new_target'])
    return NativePointers(targets, kernel_sites, kernel_groups, cut_sites)


def rebase_headers(image: CoalescedImage, cache: Kernelcache, slide: int) -> None:
    # Mach-O VM addresses are not all included in the cache's pointer list.
    # Rebase these headers explicitly; preserve section flags and offsets.
    for identifier, module in cache.modules.items():
        old_header = cache.entries[identifier]['_PrelinkExecutableLoadAddr']
        original_header = backed_offset(cache.parent, old_header, 32, len(cache.data))
        new_header = image.new_offset(old_header + slide, 32)
        for segment in module['segments']:
            if segment['va']:
                at = new_header + segment['command_offset'] - original_header
                image.write('<Q', at + 24, segment['va'] + slide)
            for section in segment['sections']:
                if section['va']:
                    at = new_header + section['command_offset'] - original_header
                    image.write('<Q', at + 32, section['va'] + slide)


def module_entrypoints(
    image: CoalescedImage, cache: Kernelcache, components: list[str], slide: int
) -> dict[str, dict[str, int]]:
    entrypoints = {}
    for identifier in components:
        va = cache.entries[identifier]['_PrelinkKmodInfo']
        at = backed_offset(cache.parent, va, 196, len(cache.data))
        start, stop = struct.unpack_from('<QQ', cache.data, at + 180)
        # The original OSKext boot path slides this field separately from its
        # coalesced pointer table. An ordinary relink must do the same.
        (address,) = struct.unpack_from('<Q', cache.data, at + 156)
        if address:
            if address != cache.entries[identifier]['_PrelinkExecutableLoadAddr']:
                raise ValueError('Original kmod address disagrees with its component header')
            image.write('<Q', image.new_offset(va + slide + 156, 8), address + slide)
        entrypoints[identifier] = {
            'kmod_info_va': va + slide,
            'start_va': start + slide if start else 0,
            'stop_va': stop + slide if stop else 0,
        }
    return entrypoints


def sparse_index(
    image: CoalescedImage, cache: Kernelcache, report: dict[str, object], slide: int
) -> tuple[dict[str, object], int]:
    headers = sorted(cache.entries[name]['_PrelinkExecutableLoadAddr'] + slide for name in report['components'])
    prelink_text = next(s for s in image.new_segments if s['name'] == '__PRELINK_TEXT')
    index_size = len(headers) * 8
    index_va = (prelink_text['va'] + prelink_text['size'] - index_size) & ~7
    occupied_end = max(
        [
            s['va'] + slide + s['size']
            for module in cache.modules.values()
            for s in module['segments']
            if s['name'] == '__TEXT'
        ]
        + [
            cache.entries[name]['_PrelinkExecutableLoadAddr'] + slide + cache.entries[name]['_PrelinkExecutableSize']
            for name in report['kernel_interfaces']
        ]
    )
    if index_va < occupied_end:
        raise ValueError('No readonly prelink padding for the sparse header index')
    index_at = image.new_offset(index_va, index_size)
    image.write(f'<{len(headers)}Q', index_at, *headers)
    return {'va': index_va, 'size': index_size, 'fileoff': None, 'headers': headers, 'abi': 1}, index_at


def selected_entries(
    cache: Kernelcache, report: dict[str, object], early_security_init: dict[str, int], slide: int
) -> list[dict[str, object]]:
    selected = []
    for identifier in [*report['kernel_interfaces'], *report['components'], *report['codeless_components']]:
        entry = copy.deepcopy(cache.entries[identifier])
        if '_NativePrelinkEarlySecurityInit' in entry:
            raise ValueError('Refuse an initializer supplied by input metadata')
        if identifier in early_security_init:
            entry['_NativePrelinkEarlySecurityInit'] = early_security_init[identifier] + slide
        for key in ('_PrelinkExecutableLoadAddr', '_PrelinkExecutableSourceAddr', '_PrelinkKmodInfo'):
            if entry.get(key):
                entry[key] += slide
        for component, dependency in CUT_DEPENDENCIES:  # OSKext would otherwise refuse the load
            if component == identifier:
                del entry['OSBundleLibraries'][dependency]
        for name in DEFERRED_PERSONALITIES.get(identifier, ()):
            del entry['IOKitPersonalities'][name]
        selected.append(entry)
    return selected


def kaslr_offsets(image: CoalescedImage, report: dict[str, object]) -> list[int]:
    new_base = next(s['va'] for s in image.new_segments if s['name'] == '__PRELINK_TEXT')
    offsets = [b['new_site'] - new_base for b in report['bindings']]
    if any(not 0 <= offset <= 0xFFFFFFFF for offset in offsets):
        raise ValueError('Rebased KASLR offset cannot fit original 32-bit format')
    return offsets


def prelink_info_xml(entries: list[dict[str, object]], offsets: list[int]) -> tuple[bytes, bytes]:
    new_metadata = {
        '_PrelinkInfoDictionary': entries,
        '_PrelinkLinkKASLROffsets': struct.pack(f'<{len(offsets) + 1}I', len(offsets), *offsets),
        '_PrelinkKCID': KCID_STAND_IN,
    }
    xml = ET.tostring(to_xml(new_metadata), encoding='utf-8').replace(b' />', b'/>') + b'\0'
    kcid = b'<key>_PrelinkKCID</key><data>' + base64.b64encode(KCID_STAND_IN) + b'</data>'
    if xml.count(kcid) != 1:
        raise ValueError('The prelink info does not hold its KCID exactly once')
    xml_head, _, xml_tail = xml.partition(kcid)
    xml_head += kcid[: -len(b'</data>') - 24]
    xml_tail = b'</data>' + xml_tail
    return xml_head, xml_tail


def executable_hashes(
    image: CoalescedImage, cache: Kernelcache, native_sites: NativeSites, inherited: dict[str, object], slide: int
) -> dict[str, str]:
    # No executable instruction byte is patched, including original POST code.
    text_hashes = {}
    for identifier, module in cache.modules.items():
        for segment in module['segments']:
            if segment['name'] == '__TEXT_EXEC' and segment['filesize']:
                old = backed_offset(cache.parent, segment['va'], segment['filesize'], len(cache.data))
                new = image.new_offset(segment['va'] + slide, segment['filesize'])
                if image.output[new : new + segment['filesize']] != cache.data[
                    old : old + segment['filesize']
                ] or native_sites.hits(new, segment['filesize']):
                    raise ValueError('Original executable bytes changed')
                text_hashes[identifier] = (
                    hashlib.sha256(cache.data[old : old + segment['filesize']]).hexdigest()
                    if identifier in NEW_ORIGINAL_COMPONENTS
                    else inherited['original_executable_sha256'][identifier]
                )
    if {k: v for k, v in text_hashes.items() if k not in NEW_ORIGINAL_COMPONENTS} != inherited[
        'original_executable_sha256'
    ] or set(text_hashes) != set(inherited['original_executable_sha256']) | (
        NEW_ORIGINAL_COMPONENTS & cache.modules.keys()
    ):
        raise ValueError('Original executable identity set changed')
    return text_hashes


class NativeImage:
    def __init__(self, native_data: bytes, native: dict[str, object]):
        self.native = native
        self.output = bytearray(native_data)
        self.new_segments = []

    def append_segment(
        self, name: str, va: int, blob: bytes | memoryview, size: int, protection: int
    ) -> dict[str, object]:
        if va % VM_PAGE or size % VM_PAGE or len(blob) > size:
            raise ValueError('Coalesced segment must be whole VM pages')
        if any(
            s['size'] and va < s['va'] + s['size'] and s['va'] < va + size
            for s in [*self.native['segments'], *self.new_segments]
        ):
            raise ValueError('Coalesced segment collides with existing mapping')
        placeholder = next((s for s in self.native['segments'] if s['name'] == name), None)
        if placeholder is None or placeholder['size'] or len(placeholder['sections']) != 1:
            raise ValueError(f'Expected one empty native {name} segment/section')
        fileoff = align(len(self.output))
        self.output.extend(bytes(fileoff - len(self.output)))
        self.output.extend(blob)
        self.output.extend(bytes(size - len(blob)))
        command = placeholder['command_offset']
        struct.pack_into('<4Q2I', self.output, command + 24, va, size, fileoff, size, protection, protection)
        section = placeholder['sections'][0]['command_offset']
        struct.pack_into('<QQI', self.output, section + 32, va, size, fileoff)
        result = {'name': name, 'va': va, 'size': size, 'filesize': size, 'fileoff': fileoff}
        self.new_segments.append(result)
        return result

    def append_plan_segments(self, plan: Plan) -> int:
        base = None
        for name, va, size, protection, at in plan.segments:
            segment = self.append_segment(name, va, memoryview(plan.blob)[at : at + size], size, protection)
            if base is None:
                base = segment['fileoff'] - at
            if segment['fileoff'] - at != base:
                raise ValueError('Coalesced segments are not contiguous')
        return base


def apply(
    plan: Plan,
    native_data: bytes,
    report: dict[str, object] | None = None,
    unbound: dict[str, list[tuple]] | None = None,
) -> bytes:
    """Run emit()'s part that reads the native kernel: the candidate from a plan.

    Given prepare's report and unbound pointers, it completes the report too.
    """
    native = macho(native_data)
    if native['kind'] != 2:
        raise ValueError('Expected MH_EXECUTE kernel images')
    addresses, stubs, exported = native_bindings(
        native_data, native, [tuple(t) for t in plan.targets], [tuple(c[1:]) for c in plan.cuts], plan.extents
    )
    check_native_abi(native_data, native, exported, early_security_init=plan.early_security_init)
    image = NativeImage(native_data, native)
    base = image.append_plan_segments(plan)
    pack = struct.Struct('<Q').pack_into
    for at, group in zip(plan.kernel_sites, plan.kernel_groups, strict=False):
        pack(image.output, base + at, addresses[group])
    for (at, *_), address in zip(plan.cuts, stubs, strict=False):
        pack(image.output, base + at, address)
    kcid = hashlib.sha256(KCID_DOMAIN)
    kcid.update(native_data)
    kcid.update(bytes.fromhex(plan.original_sha256))
    xml = plan.xml_head + base64.b64encode(kcid.digest()[:16]) + plan.xml_tail
    info_va = align(max(s['va'] + s['size'] for s in native['segments'] if s['size']), VM_PAGE)
    image.append_segment('__PRELINK_INFO', info_va, xml, align(len(xml), VM_PAGE), 3)
    add_sparse_index_section(image, plan.index, base)
    macho(image.output)
    if report is not None:
        bind_report(report, unbound, plan, addresses, stubs)
        report['native_sha256'] = hashlib.sha256(native_data).hexdigest()
        report['sparse_index']['fileoff'] = base + plan.index[2]
        report.update(
            candidate_sha256=hashlib.sha256(image.output).hexdigest(),
            coalesced_segments=image.new_segments,
            candidate_bytes=len(image.output),
        )
    return bytes(image.output)


def check_native_abi(
    native_data: bytes, native: dict[str, object], exported: dict[int, list[str]], *, early_security_init: bool
) -> None:
    marker = [address for address, names in exported.items() if '_kalloc_sparse_prelink_index_version' in names]
    if len(marker) != 1:
        raise ValueError('Native kernel lacks sparse-prelink index ABI')
    marker_at = backed_offset(native['segments'], marker[0], 4, len(native_data))
    if struct.unpack_from('<I', native_data, marker_at)[0] != 1:
        raise ValueError('Unsupported native sparse-prelink index ABI')
    if early_security_init:
        markers = [address for address, names in exported.items() if '_native_prelink_security_init_version' in names]
        if len(markers) != 1:
            raise ValueError('Native kernel lacks early security initializer ABI')
        at = backed_offset(native['segments'], markers[0], 4, len(native_data))
        if struct.unpack_from('<I', native_data, at)[0] != 1:
            raise ValueError('Unsupported early security initializer ABI')


def add_sparse_index_section(image: NativeImage, index: list[int], base: int) -> None:
    # Expand the existing command in verified Mach-O header padding; mapped
    # payload offsets and all original kernel/component instruction bytes stay put.
    index_va, index_size, index_at = index
    prelink_text = next(s for s in image.new_segments if s['name'] == '__PRELINK_TEXT')
    commands = []
    for command, at, length in image.native['commands']:
        blob = bytearray(image.output[at : at + length])
        if command == 0x19 and blob[8:24].rstrip(b'\0') == b'__PRELINK_TEXT':
            struct.pack_into('<I', blob, 4, length + 80)
            struct.pack_into('<I', blob, 64, 2)
            struct.pack_into('<Q', blob, 72 + 40, index_va - prelink_text['va'])
            blob.extend(
                struct.pack(
                    '<16s16sQQ8I',
                    b'__macho_list',
                    b'__PRELINK_TEXT',
                    index_va,
                    index_size,
                    base + index_at,
                    3,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                )
            )
        commands.append(blob)
    rebuilt = b''.join(commands)
    first_payload = min(
        section['fileoff']
        for segment in image.native['segments']
        for section in segment['sections']
        if section['size'] and section['fileoff']
    )
    if 32 + len(rebuilt) > first_payload:
        raise ValueError('Insufficient native Mach-O header padding for sparse index section')
    struct.pack_into('<I', image.output, 20, len(rebuilt))
    image.output[32 : 32 + len(rebuilt)] = rebuilt


def bind_report(
    report: dict[str, object], unbound: dict[str, list[tuple]], plan: Plan, addresses: list[int], stubs: list[int]
) -> None:
    bindings = report['bindings']
    for (index, target, _), group in zip(unbound['kernel'], plan.kernel_groups, strict=False):
        if plan.targets[group][0] != target:
            raise ValueError('The plan and the report disagree on a kernel pointer')
        bindings[index]['new_target'] = addresses[group]
    for (index, *_), address in zip(unbound['cut'], stubs, strict=False):
        bindings[index]['new_target'] = address


def emit(native_data: bytes, original: Original) -> tuple[bytes, dict[str, object]]:
    """Return the candidate and its report, in one process."""
    plan, report, unbound = prepare(original)
    return apply(plan, native_data, report, unbound), report


def original_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument('--prelinked', required=True, type=Path)
    parser.add_argument('--options', type=Path, metavar='PRELINK_JSON')
    parser.add_argument('--root', action='append', default=[])
    parser.add_argument('--new-root', action='append', default=[])
    parser.add_argument('--cut', action='append', default=[], metavar='COMPONENT=DEPENDENCY=SITES')
    parser.add_argument('--defer', action='append', default=[], metavar='COMPONENT=PERSONALITY')
    parser.add_argument('--slide', type=lambda value: int(value, 0))
    parser.add_argument(
        '--early-security-init',
        action='append',
        default=[],
        metavar='BUNDLE=ORIGINAL_VA',
        help='Explicit original void(void) policy initializer, called before lockdown; requires source ABI',
    )
    parser.add_argument('--inherit-original-report', required=True, type=Path)
    parser.add_argument(
        '--previous',
        required=True,
        type=Path,
        help='the V20 prelink: its roots and the sha256s of its kernelcache and report',
    )


def take_options(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    """Fill the original arguments from --options's prelink.json, or check they were given as flags."""
    flags = (args.root, args.new_root, args.cut, args.defer, args.early_security_init)
    if not args.options:
        if not args.root or args.slide is None:
            parser.error('--root and --slide, or --options, are required')
        return
    if any(flags) or args.slide is not None:
        parser.error('--options stands for --root, --new-root, --cut, --defer, --slide and --early-security-init')
    options = json.loads(args.options.read_text())
    args.root = options['base_roots'] + options['new_roots']
    args.new_root = options['new_roots']
    args.cut = options['cuts']
    args.defer = options['defers']
    args.slide = int(options['slide'], 0)
    args.early_security_init = options['early_security_init']


def original_inputs(
    parser: argparse.ArgumentParser, args: argparse.Namespace
) -> tuple[bytes, dict[str, object], dict[str, int], dict[str, object]]:
    """Return the checked original inputs: (kernelcache bytes, inherited report, early initializers, previous)."""
    take_options(parser, args)
    NEW_ORIGINAL_COMPONENTS.update(args.new_root)
    for cut in args.cut:
        component, dependency, sites = cut.split('=')
        CUT_DEPENDENCIES[(component, dependency)] = (int(sites), CUT_STUB if int(sites) else None)
    for defer in args.defer:
        component, personality = defer.split('=')
        DEFERRED_PERSONALITIES[component] = (*DEFERRED_PERSONALITIES.get(component, ()), personality)
    initializers = {}
    for request in args.early_security_init:
        identifier, separator, address = request.partition('=')
        if not separator or not identifier or identifier in initializers:
            parser.error('Use one BUNDLE=ORIGINAL_VA initializer per module')
        initializers[identifier] = int(address, 0)
    previous = json.loads(args.previous.read_text())
    report_bytes = args.inherit_original_report.read_bytes()
    if hashlib.sha256(report_bytes).hexdigest() != previous['report_sha256']:
        raise ValueError('Only the completed V20 original-component report may be inherited')
    prelinked = args.prelinked.read_bytes()
    if hashlib.sha256(prelinked).hexdigest() != previous['prelinked_sha256'] or args.root != previous['roots'] + sorted(
        NEW_ORIGINAL_COMPONENTS
    ):
        raise ValueError('Only the unchanged V20 roots are allowed')
    inherited = json.loads(report_bytes)
    if len(inherited['components']) != 33:
        raise ValueError('Expected 33 original components')
    return prelinked, inherited, initializers, previous


def main() -> None:
    mode = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] in ('plan', 'apply', 'report') else None
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    if mode == 'apply':
        parser.add_argument('--plan', required=True, type=Path)
        parser.add_argument('--kernel', required=True, type=Path)
        parser.add_argument('--output', required=True, type=Path)
        args = parser.parse_args(sys.argv[2:])
        image = apply(Plan.load(args.plan), args.kernel.read_bytes())
        if args.output.exists():
            raise SystemExit('Refuse to overwrite an existing candidate')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(image)
        print(f'Experimental candidate: {len(image)} bytes; executable bytes unchanged; runtime unqualified')
        return
    original_arguments(parser)
    if mode == 'plan':
        parser.add_argument('--plan-output', required=True, type=Path)
    else:
        parser.add_argument('--kernel', required=True, type=Path)
        if mode == 'report':
            parser.add_argument('--expect', required=True, type=Path, help='the candidate apply made from the plan')
            parser.add_argument('--report-output', required=True, type=Path)
        else:
            parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args(sys.argv[2:] if mode else sys.argv[1:])
    prelinked, inherited, initializers, previous = original_inputs(parser, args)
    original = Original(prelinked, args.root, args.slide, initializers, inherited)
    if mode == 'plan':
        plan, _, _ = prepare(original)
        args.plan_output.parent.mkdir(parents=True, exist_ok=True)
        plan.save(args.plan_output)
        print(
            f'Prelink plan: {len(plan.blob)} bytes of coalesced segments, {len(plan.kernel_sites)} kernel and '
            f'{len(plan.cuts)} cut pointers to bind'
        )
        return
    image, report = emit(args.kernel.read_bytes(), original)
    report['original_input_validation']['inherited_report'] = (
        'native-root-dma-v1/kernel.json sha256 ' + previous['report_sha256']
    )
    if mode == 'report':
        if args.expect.read_bytes() != image:
            raise SystemExit(f'{args.expect}: not the candidate emit makes from these inputs')
        args.report_output.parent.mkdir(parents=True, exist_ok=True)
        args.report_output.write_text(json.dumps(report, indent=2) + '\n')
        return
    if args.output.exists():
        raise SystemExit('Refuse to overwrite an existing candidate')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Experimental candidate: {len(image)} bytes; executable bytes unchanged; runtime unqualified')


if __name__ == '__main__':
    main()
