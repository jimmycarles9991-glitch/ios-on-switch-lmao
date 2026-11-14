"""Every way the plugin's own code reaches m2v, read from its sources (m2v_routes.tsv's product rows are this set).

A route is (route, name) with the files that raise it. The m2v device comes only from rmM2V (an RMNeed) or
rmM2VIfOpen, its objects only from that device, from a client's foreign object (a predicate) or from a twin field, and
its code only through a bridge symbol, an NB class looked up by name, an NVK export or a header of the m2v tree, so
these detectors close the set together with the no-m2v link lists (gpu/m2v_link): a call the sources hide behind a
pointer is a link edge.
"""

import re
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).parents[5]
METAL = ROOT / 'drivers/TegraGM20BMetal'
RUNTIME = METAL / 'raw/runtime'
KERNEL = ROOT / 'drivers/TegraGM20B'
INVENTORY = METAL / 'raw/test/inventory.tsv'
SUFFIXES = {'.mm', '.cpp', '.h', '.c'}

# The fields that hold an m2v object or the device, the functions that hand one over or replay onto it, and the
# forwarding machinery, each by the code (strings and comments blanked) that names it
M2V_IDENTS = {
    'rmM2VIfOpen': r'\brmM2VIfOpen\b',
    'fallback': r'(?:->|\.)fallback\b|id<MTLDevice>\s+fallback\b',
    'twinOf': r'\btwinOf\b',
    'rmTwin': r'\brmTwin\b',
    '_twin': r'\b_twin\b',
    '_m2v': r'\b_m2v\b',
    '_compatibility': r'\b_compatibility\b',
    'RMTwinObject': r'\bRMTwinObject\b',
    'wrapCarried': r'\bwrapCarried\b',
    'twinAnswers': r'\btwinAnswers\b',
    'answerMissing': r'\banswerMissing\b',
    'installM2VForwarders': r'\binstallM2VForwarders\b',
    '_objc_msgForward': r'\b_objc_msgForward\b',
    'replay': r'(?<![.:>\w])replay\s*\(\s*(?:RMCommandBuffer\b|cb\s*,)',
    'replayPlannedState': r'\breplayPlannedState\b',
    'replay*': r'\breplay(?!PlannedState\b|TwinUs\b)[A-Z]\w*',
    'rmRefuseM2vRange': r'\brmRefuseM2vRange\b',
    'rmM2vMissingState': r'\brmM2vMissing(?:State|Kernel)\b',
    'range.raw=false': r'\brange\.raw\s*=\s*false\b',
    'takeReason': r'\btake(?:Whole)?Reason\b',
}
# The kernel's submission entries only NVK calls (TegraGM20BABI.h)
KERNEL_LEGACY = ('Exec', 'ExecSurfaces', 'Submit', 'SubmitAddress')

TWIN = re.compile(r'\b\w*[Tt]win\w*\b')
BRIDGE = re.compile(r'\b(native_bridge_metal_\w+|nmb_\w+|switch_nmb_\w+)\b')
NB_CLASS = re.compile(r'\b(NB[A-Z]\w*)\b')
INCLUDE = re.compile(r'#\s*(?:include|import)\s*"([^"]+)"')
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
NVK_NAME = re.compile(r'^(switch_(?:nvk|nak)_\w+|vk\w+|nvk_\w+|[\w.]*vulkan[\w.]*\.dylib)$')
DL_CALL = re.compile(r'\bdl(?:sym|open)\s*\(([^;]*?)\)\s*[;),]')
TAG_FORMS = (
    re.compile(r'\bSWITCH_REFUSE(?:_DETAIL)?\s*\(\s*"([^"]+)"\s*,\s*"([^"]+)"'),
    re.compile(r'\bSWITCH_REFUSAL_KEY\s*\(\s*"([^"]+)"\s*,\s*"([^"]+)"'),
    re.compile(r'\bswitch_refusal(?:_log|_key)?\s*\(\s*"([^"]+)"\s*,\s*"([^"]+)"'),
    re.compile(r'\bswitch_refusal_k\s*\([^;]*?,\s*"([^"]+)"\s*,\s*"([^"]+)"'),
)


def strip_comments(text: str, strings: bool = True) -> str:
    """Blank the // and /* */ comments of `text`, keeping its newlines.

    String and character literals stay whole, or are emptied when `strings` is false.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i : j + 1] if strings else c + c)
            i = j + 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def product_units() -> list[Path]:
    """List the plugin's own sources and the shader compiler's (never the m2v frontend or the bridge)."""
    units = []
    for directory in (RUNTIME, METAL / 'raw/fx', METAL / 'compiler'):
        units += [p for p in sorted(directory.iterdir()) if p.suffix in SUFFIXES]
    return [*units, METAL / 'TegraGM20BMetalPlugin.mm']


def kernel_units() -> list[Path]:
    return [p for p in sorted(KERNEL.iterdir()) if p.suffix in SUFFIXES]


def enum_pairs(header: str, enum: str, table: str, skip: tuple[str, ...]) -> list[tuple[str, str]]:
    """Pair each enumerator of `enum` before its count with its name in `table`, leaving out those in `skip`."""
    text = strip_comments(header)
    body = re.search(r'enum ' + enum + r'\s*:\s*\w+\s*\{(.*?)\};', text, re.DOTALL).group(1)
    enumerators = [e.split('=')[0].strip() for e in body.split(',') if e.strip()][:-1]
    names = LITERAL.findall(re.search(table + r'\[[^\]]*\]\s*=\s*\{(.*?)\};', text, re.DOTALL).group(1))
    if len(enumerators) != len(names):
        raise ValueError(f'{enum}: {len(enumerators)} enumerators before the count, {len(names)} names')
    return [(e, n) for e, n in zip(enumerators, names, strict=True) if e not in skip]


class Census:
    def __init__(self) -> None:
        sources = {p: p.read_text(errors='replace') for p in product_units()}
        self.texts = {p: strip_comments(t) for p, t in sources.items()}
        self.code = {p: strip_comments(t, strings=False) for p, t in sources.items()}
        self.routes: dict[tuple[str, str], set[str]] = defaultdict(set)
        self.preds = enum_pairs(
            (RUNTIME / 'RawMetalPredicates.h').read_text(), 'RMPred', 'kPredNames', ('PredNone', 'PredCount')
        )
        self.needs = enum_pairs((RUNTIME / 'RawMetalNeeds.h').read_text(), 'RMNeed', 'kNeedNames', ('NeedCount',))
        self._enumerators('pred', self.preds, 'RawMetalPredicates.h')
        self._enumerators('need', self.needs, 'RawMetalNeeds.h')
        for path, text in self.texts.items():
            self._scan(path.name, text, self.code[path])
        self._selectors()
        self._kernel()

    def add(self, route: str, name: str, file: str) -> None:
        self.routes[(route, name)].add(file)

    def _enumerators(self, route: str, pairs: list[tuple[str, str]], header: str) -> None:
        for enumerator, name in pairs:
            word = re.compile(r'\b' + enumerator + r'\b')
            self.routes[(route, name)]
            for path, text in self.code.items():
                if path.name != header and word.search(text):
                    self.add(route, name, path.name)

    def _scan(self, file: str, text: str, code: str) -> None:
        for form in TAG_FORMS:
            for domain, reason in form.findall(text):
                self.add('tag', f'{domain}:{reason}', file)
        for symbol in BRIDGE.findall(code):
            self.add('bridge', symbol, file)
        for cls in NB_CLASS.findall(text):
            self.add('class', cls, file)
        for header in INCLUDE.findall(text):
            if header.startswith('bridge/') or Path(header).name == 'NativeBridgeMetalFrontend.h':
                self.add('include', header, file)
        self._names(file, text, code)
        for ident, pattern in M2V_IDENTS.items():
            if re.search(pattern, code):
                self.add('ident', ident, file)

    def _names(self, file: str, text: str, code: str) -> None:
        for word in set(TWIN.findall(code)):
            self.add('twin', word, file)
        for literal in LITERAL.findall(text):
            if NVK_NAME.match(literal):
                self.add('nvk', literal, file)
        for call in DL_CALL.findall(text):
            for literal in LITERAL.findall(call):
                self.add('dlsym', literal, file)

    def _selectors(self) -> None:
        for line in INVENTORY.read_text().splitlines():
            fields = line.split('\t')
            if line.startswith('#') or len(fields) != 6 or fields[5] not in ('m2v', 'm2vobj'):
                continue
            self.add('sel', f'{fields[0]} {fields[2]}', INVENTORY.name)

    def _kernel(self) -> None:
        texts = {
            p.name: strip_comments(p.read_text(errors='replace'), strings=False)
            for p in kernel_units()
            if p.name != 'TegraGM20BABI.h'
        }
        texts.update({p.name: t for p, t in self.code.items()})
        for method in KERNEL_LEGACY:
            word = re.compile(r'\bTegraGM20BMethod' + method + r'\b')
            self.routes[('kernel', method)]
            for file, text in texts.items():
                if word.search(text):
                    self.add('kernel', method, file)
