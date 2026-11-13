"""What the chip takes in an iterated-blend program, from the 12 Nov device sitting (x1-bench3/BENCH3.md).

The first pass may read only S, D, their alphas, ONE, ZERO, the 1 - alpha operands and constant 0: PBR, a temp or
another constant there is INVALID_VALUE at the load. The instruction pointer takes 0 only. ALPHA_ENABLE wants an R_TO_A
pass (ITERATED_BLEND_ALPHA_ILLEGAL_R_TO_A without one) and an R_TO_A pass wants ALPHA_ENABLE
(ITERATED_BLEND_R_TO_A_WITHOUT_ITERATED_ALPHA). A program whose first pass reads constant k runs with constants 0 and k
swapped instead.
"""

SET_ITERATED_BLEND = 0x0DD0
LOAD_POINTER = 0x0DF0
LOAD_INSTRUCTION = 0x0DF4
CONSTANT_RED = 0x1480
CONSTANTS_END = 0x1500
ALPHA_ENABLE = 1 << 1
R_TO_A = 5
SELECT_SHIFT, SELECT_MASK = 6, 0x7
A_TEMPS_AND_PBR = range(4, 8)
BD_TEMPS_AND_PBR = range(9, 13)
BD_CONSTANT = 13


def _field(word: int, lo: int, bits: int) -> int:
    return word >> lo & ((1 << bits) - 1)


def first_pass_refusal(word: int) -> str | None:
    a, b, c, d = _field(word, 9, 3), _field(word, 12, 4), _field(word, 16, 3), _field(word, 19, 4)
    if a in A_TEMPS_AND_PBR or c in A_TEMPS_AND_PBR or b in BD_TEMPS_AND_PBR or d in BD_TEMPS_AND_PBR:
        return 'the first pass reads PBR or a temp'
    if BD_CONSTANT in (b, d) and _field(word, SELECT_SHIFT, 3):
        return 'the first pass reads a constant other than 0'
    return None


def _loaded(methods: list[list[int]]) -> tuple[list[int], int, str | None]:
    """Give the words loaded, the last SET_ITERATED_BLEND and why a pointer or a first pass is refused."""
    words, enable, why, first = [], 0, None, True
    for m, v in methods:
        if m == LOAD_POINTER:
            why, first = why or ('LOAD_ITERATED_BLEND_INSTRUCTION_POINTER takes 0 only' if v else None), True
        elif m == LOAD_INSTRUCTION:
            why = why or (first_pass_refusal(v) if first else None)
            words.append(v)
            first = False
        elif m == SET_ITERATED_BLEND:
            enable = v
    return words, enable, why


def refusal(methods: list[list[int]]) -> str | None:
    """Give why the chip would refuse the program these methods load, or None."""
    words, enable, why = _loaded(methods)
    if why or not enable or not words:
        return why
    to_alpha = any(_field(w, 23, 3) == R_TO_A for w in words)
    if enable & ALPHA_ENABLE and not to_alpha:
        return 'ALPHA_ENABLE without an R_TO_A pass'
    if to_alpha and not enable & ALPHA_ENABLE:
        return 'an R_TO_A pass without ALPHA_ENABLE'
    return None


def _swap(j: int, a: int, b: int) -> int:
    return b if j == a else a if j == b else j


def renumbered(methods: list[list[int]]) -> list[list[int]]:
    """Give the methods with constant 0 and the one the first pass reads swapped, everywhere they are named."""
    first = next((v for m, v in methods if m == LOAD_INSTRUCTION), None)
    if first is None or BD_CONSTANT not in (_field(first, 12, 4), _field(first, 19, 4)):
        return methods
    k = _field(first, SELECT_SHIFT, 3)
    if not k:
        return methods
    out = []
    for method, value in methods:
        m, v = method, value
        if m == LOAD_INSTRUCTION:
            j = _swap(_field(v, SELECT_SHIFT, 3), 0, k)
            v = v & ~(SELECT_MASK << SELECT_SHIFT) | j << SELECT_SHIFT
        elif CONSTANT_RED <= m < CONSTANTS_END:
            j, lane = divmod(m - CONSTANT_RED, 16)
            m = CONSTANT_RED + 16 * _swap(j, 0, k) + lane
        out.append([m, v])
    return out
