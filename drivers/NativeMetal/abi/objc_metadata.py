"""The Objective-C metadata of an image in a dyld shared cache: its classes, protocols and selector references.

A method list in the shared cache is relative (its entries are 32-bit offsets), and its flags say from where: a
name is an offset from the cache's selector base (the ObjC optimization header's relativeMethodSelectorBase, an
offset from the cache's base) or from the field to a selector reference; a type string is an offset from the
selector base (iOS 27's caches) or from the field. A class's method or protocol list with its low bit set is a list
of lists: the class's own and those of the categories the cache builder attached to it.

Layouts: objc4's objc-runtime-new.h (class_t, class_ro_t, method_list_t, ivar_list_t, protocol_t,
relative_list_list_t) and dyld's objc-shared-cache.h (ObjCOptimizationHeader).
"""

from dataclasses import dataclass, field

from drivers.NativeMetal.abi.dyld_cache import DyldCache

CLASS_SUPERCLASS, CLASS_DATA = 8, 32
DATA_FLAG_BITS = 7
RO_INSTANCE_START, RO_INSTANCE_SIZE, RO_NAME, RO_METHODS, RO_PROTOCOLS, RO_IVARS = 4, 8, 24, 32, 40, 48
PROTOCOL_NAME, PROTOCOL_PROTOCOLS, PROTOCOL_INSTANCE, PROTOCOL_CLASS, PROTOCOL_OPTIONAL_INSTANCE = 8, 16, 24, 32, 40
PROTOCOL_SIZE, PROTOCOL_EXTENDED_TYPES = 64, 72
SMALL_METHODS = 0x80000000
NAMES_FROM_SELECTOR_BASE = 0x40000000
TYPES_FROM_SELECTOR_BASE = 0x20000000
ENTSIZE_MASK = 0xFFFC
LIST_OF_LISTS = 1
OBJC_OPTS_SELECTOR_BASE = 0x30
IVAR_BYTES = 32


@dataclass(frozen=True)
class Ivar:
    name: str
    type: str
    offset: int
    size: int


@dataclass(frozen=True)
class Method:
    name: str
    types: str


@dataclass
class ObjcClass:
    name: str
    superclass: str | None
    instance_start: int
    instance_size: int
    ivars: list[Ivar] = field(default_factory=list)
    methods: list[Method] = field(default_factory=list)
    protocols: list[str] = field(default_factory=list)


@dataclass
class ObjcProtocol:
    name: str
    protocols: list[str] = field(default_factory=list)
    required: list[Method] = field(default_factory=list)
    optional: list[Method] = field(default_factory=list)


class ObjcReader:
    def __init__(self, cache: DyldCache):
        self.cache = cache
        self.selector_base = cache.base + cache.u64(cache.objc_opts + OBJC_OPTS_SELECTOR_BASE)

    def _lists(self, address: int) -> list[int]:
        c = self.cache
        if not address & LIST_OF_LISTS:
            return [address] if address else []
        base = address & ~LIST_OF_LISTS
        size, count = c.u32(base) & ENTSIZE_MASK, c.u32(base + 4)
        lists = []
        for i in range(count):
            entry = base + 8 + size * i
            raw = c.u64(entry)
            lists.append(entry + (raw >> 16) - ((raw >> 63) << 48))
        return lists

    def _methods(self, address: int) -> list[Method]:
        return [m for at in self._lists(address) for m in self._method_list(at)]

    def _method_list(self, address: int) -> list[Method]:
        c = self.cache
        flags, count = c.u32(address), c.u32(address + 4)
        size = flags & ENTSIZE_MASK
        methods = []
        for i in range(count):
            entry = address + 8 + size * i
            if not flags & SMALL_METHODS:
                methods.append(Method(c.cstring(c.pointer(entry)), c.cstring(c.pointer(entry + 8))))
                continue
            name = c.i32(entry)
            name = self.selector_base + name if flags & NAMES_FROM_SELECTOR_BASE else c.pointer(entry + name)
            types = c.i32(entry + 4)
            types = self.selector_base + types if flags & TYPES_FROM_SELECTOR_BASE else entry + 4 + types
            methods.append(Method(c.cstring(name), c.cstring(types)))
        return methods

    def _count(self, address: int) -> int:
        return sum(self.cache.u32(at + 4) for at in self._lists(address))

    def _protocol_names(self, address: int) -> list[str]:
        c = self.cache
        return [
            c.cstring(c.pointer(c.pointer(at + 8 + 8 * i) + PROTOCOL_NAME))
            for at in self._lists(address)
            for i in range(c.u64(at))
        ]

    def _ivars(self, address: int) -> list[Ivar]:
        if not address:
            return []
        c = self.cache
        ivars = []
        for i in range(c.u32(address + 4)):
            entry = address + 8 + IVAR_BYTES * i
            offset_at = c.pointer(entry)
            ivars.append(
                Ivar(
                    name=c.cstring(c.pointer(entry + 8)),
                    type=c.cstring(c.pointer(entry + 16)),
                    offset=c.u32(offset_at) if offset_at else 0,
                    size=c.u32(entry + 28),
                )
            )
        return ivars

    def _ro(self, cls: int) -> int:
        return self.cache.pointer(cls + CLASS_DATA) & ~DATA_FLAG_BITS

    def class_name(self, cls: int) -> str:
        return self.cache.cstring(self.cache.pointer(self._ro(cls) + RO_NAME))

    def superclass(self, cls: int) -> int:
        return self.cache.pointer(cls + CLASS_SUPERCLASS)

    def read_class(self, cls: int) -> ObjcClass:
        c = self.cache
        ro = self._ro(cls)
        superclass = self.superclass(cls)
        return ObjcClass(
            name=c.cstring(c.pointer(ro + RO_NAME)),
            superclass=self.class_name(superclass) if superclass else None,
            instance_start=c.u32(ro + RO_INSTANCE_START),
            instance_size=c.u32(ro + RO_INSTANCE_SIZE),
            ivars=self._ivars(c.pointer(ro + RO_IVARS)),
            methods=self._methods(c.pointer(ro + RO_METHODS)),
            protocols=self._protocol_names(c.pointer(ro + RO_PROTOCOLS)),
        )

    def classes(self, image: str) -> dict[str, int]:
        """Return the classes the image defines, by name: the address of each class_t."""
        return {self.class_name(cls): cls for cls in self.cache.pointers(self.cache.section(image, '__objc_classlist'))}

    def read_protocol(self, address: int) -> ObjcProtocol:
        """Read a protocol's instance methods, typed by its extended method types where it has them.

        objc4 orders those types required instance, required class, optional instance, optional class.
        """
        c = self.cache
        required = self._methods(c.pointer(address + PROTOCOL_INSTANCE))
        optional = self._methods(c.pointer(address + PROTOCOL_OPTIONAL_INSTANCE))
        extended = c.pointer(address + PROTOCOL_EXTENDED_TYPES) if c.u32(address + PROTOCOL_SIZE) > 72 else 0
        if extended:
            optional_at = len(required) + self._count(c.pointer(address + PROTOCOL_CLASS))
            required = [Method(m.name, c.cstring(c.pointer(extended + 8 * i))) for i, m in enumerate(required)]
            optional = [
                Method(m.name, c.cstring(c.pointer(extended + 8 * (optional_at + i)))) for i, m in enumerate(optional)
            ]
        return ObjcProtocol(
            name=c.cstring(c.pointer(address + PROTOCOL_NAME)),
            protocols=self._protocol_names(c.pointer(address + PROTOCOL_PROTOCOLS)),
            required=required,
            optional=optional,
        )

    def protocols(self, image: str) -> dict[str, int]:
        """Return the protocols the image defines, by name: the address of each protocol_t."""
        c = self.cache
        return {c.cstring(c.pointer(p + PROTOCOL_NAME)): p for p in c.pointers(c.section(image, '__objc_protolist'))}

    def selector_references(self, image: str) -> set[str]:
        """Return every selector the image references (its __objc_selrefs)."""
        c = self.cache
        return {c.cstring(s) for s in c.pointers(c.section(image, '__objc_selrefs')) if s}
