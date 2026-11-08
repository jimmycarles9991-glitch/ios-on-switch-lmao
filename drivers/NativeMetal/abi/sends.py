"""The selectors an image's code sends, read from its instructions rather than its selector references.

An image's __objc_selrefs over-count: one that adopts a protocol (a Swift conformance, an ObjC class) references every
method of it. What the code sends is what it loads into x1. The cache builder rewrote those loads: an ADRP+LDR of a
selector reference became an ADRP+ADD of the uniqued selector string, and the objc_msgSend$<selector> stubs moved
into stub islands outside the image, where each loads its selector the same way. So the scan follows every ADRP+ADD
or ADRP+LDR of the image's own text to a selector, and every branch out of the image to a stub that loads x1 first.
"""

import array

from drivers.NativeMetal.abi.dyld_cache import CacheError, DyldCache

ADRP_MASK, ADRP = 0x9F000000, 0x90000000
ADRP_X1_MASK, ADRP_X1 = 0x9F00001F, 0x90000001
LDR_X_IMM_MASK, LDR_X_IMM = 0xFFC00000, 0xF9400000
ADD_X_IMM_MASK, ADD_X_IMM = 0xFF800000, 0x91000000
BRANCH_MASK, B, BL = 0xFC000000, 0x14000000, 0x94000000
PAIR_WINDOW = 8
STUB_WORDS = 4
SELECTOR_LIMIT = 512


def _adrp_page(word: int, pc: int) -> int:
    imm = ((word >> 29) & 3) | (((word >> 5) & 0x7FFFF) << 2)
    if imm & (1 << 20):
        imm -= 1 << 21
    return (pc & ~0xFFF) + (imm << 12)


def _paired_address(words: array.array, at: int, base: int) -> int | None:
    """Return the address an ADRP at `at` and the LDR or ADD that completes it (within a few instructions) give."""
    page, register = _adrp_page(words[at], base + 4 * at), words[at] & 31
    for word in words[at + 1 : at + PAIR_WINDOW]:
        if (word >> 5) & 31 != register:
            continue
        if word & LDR_X_IMM_MASK == LDR_X_IMM:
            return page + ((word >> 10) & 0xFFF) * 8
        if word & ADD_X_IMM_MASK == ADD_X_IMM:
            return page + ((word >> 10) & 0xFFF)
    return None


class SendScanner:
    """Scans images of one cache; stub islands are shared by every image, so their selectors are kept."""

    def __init__(self, cache: DyldCache):
        self.cache = cache
        self._stubs: dict[int, str | None] = {}

    def _selector_at(self, address: int, selectors: dict[int, str]) -> str | None:
        if address in selectors:
            return selectors[address]
        try:
            text = self.cache.cstring(address, SELECTOR_LIMIT)
        except CacheError:
            return None
        return text if text and text.isprintable() and ' ' not in text else None

    def _stub_selector(self, address: int) -> str | None:
        if address not in self._stubs:
            selector = None
            try:
                words = array.array('I', self.cache.read(address, 4 * STUB_WORDS))
                if words[0] & ADRP_X1_MASK == ADRP_X1:
                    target = _paired_address(words, 0, address)
                    if target is not None:
                        selector = self._selector_at(target, {}) or self._selector_at(self.cache.pointer(target), {})
            except CacheError:
                pass
            self._stubs[address] = selector
        return self._stubs[address]

    def _references(self, image: str) -> dict[int, str]:
        """Each selector reference of the image and the string it points at, by both addresses."""
        section = self.cache.section(image, '__objc_selrefs')
        found: dict[int, str] = {}
        if section is None:
            return found
        for i in range(section.size // 8):
            target = self.cache.pointer(section.address + 8 * i)
            if target:
                found[target] = found[section.address + 8 * i] = self.cache.cstring(target)
        return found

    def sent(self, image: str) -> set[str]:
        selectors = self._references(image)
        found: set[str] = set()
        for section in self.cache.sections(image):
            if (section.segment, section.name) != ('__TEXT', '__text') or not section.size:
                continue
            words = array.array('I', self.cache.read(section.address, section.size))
            end = section.address + section.size
            for at, word in enumerate(words):
                if word & ADRP_MASK == ADRP:
                    target = _paired_address(words, at, section.address)
                    if target in selectors:
                        found.add(selectors[target])
                elif word & BRANCH_MASK in (B, BL):
                    offset = word & 0x3FFFFFF
                    if offset & (1 << 25):
                        offset -= 1 << 26
                    target = section.address + 4 * at + 4 * offset
                    if not section.address <= target < end and (selector := self._stub_selector(target)):
                        found.add(selector)
        return found
