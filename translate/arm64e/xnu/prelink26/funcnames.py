"""C strings an Apple kernel function addresses (ADRP then ADD): a function that passes its own __func__ to panic or
an assert names itself (thread_call_enter1_delayed's "thread_call_enter1_delayed")."""
import bisect
import struct


def _signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def strings(kc, fstarts, a):
    i = bisect.bisect_right(fstarts, a)
    end = min(fstarts[i] if i < len(fstarts) else a + 4, a + 0x2000)
    at = kc.va2off(a)
    pages, out = {}, set()
    for pc in range(a, end, 4):
        w = struct.unpack_from("<I", kc.d, at + pc - a)[0]
        if w & 0x9F000000 == 0x90000000:
            pages[w & 31] = (pc & ~0xFFF) + (_signed(((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2), 21) << 12)
        elif w & 0xFF800000 == 0x91000000 and (w >> 5) & 31 in pages:
            target = pages[(w >> 5) & 31] + ((w >> 10) & 0xFFF)
            off = kc.va2off(target)
            if off is None:
                continue
            stop = kc.d.find(b"\0", off, off + 128)
            text = kc.d[off:stop] if stop > off else b""
            if text and all(0x21 <= c < 0x7F for c in text):
                out.add(text.decode())
    return out
