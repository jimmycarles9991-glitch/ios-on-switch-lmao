"""Kernel functions that vnode operation vectors name (struct vnodeopv_entry_desc: {vnodeop_desc *, function}).

XNU's own vectors (spec, fifo, dead, devfs, bindfs) pair each desc with the same function in Apple's kernel and in
ours, so a vector of Apple's whose descs are exactly those of one of ours names its functions (spec and fifo share
their descs: the one whose functions already carry our names wins). A file system's own
vector points some descs at vfs_support.c's err_<op> or nop_<op>; the one whose body is the same instructions as
Apple's names it.
"""
import bisect
import struct

NOP, MOV_W0_WZR, MOV_W0_0 = 0xD503201F, 0x2A1F03E0, 0x52800000
PAC_HINTS = {0xD503237F, 0xD503233F, 0xD50323BF, 0xD50323FF, 0xD503239F, 0xD503219F, 0xD50321DF, 0xD503241F,
             0xD503245F, 0xD503249F, 0xD50324DF}


def _runs(pairs):
    """{desc, function} pairs at 16-byte steps as one vector each: [{desc: function}]"""
    out, last = [], None
    for site, desc, function in sorted(pairs):
        if last is None or site != last + 16:
            out.append({})
        out[-1][desc] = function
        last = site
    return out


def _body(words):
    out = []
    for w in words:
        if w in PAC_HINTS or w == NOP:
            continue
        if w == MOV_W0_WZR:
            w = MOV_W0_0
        if w in (0xD65F0FFF, 0xD65F0BFF):
            w = 0xD65F03C0
        if w & 0x7C000000 == 0x14000000:
            w &= 0xFC000000
        elif w & 0x9F000000 == 0x90000000:
            w &= 0x9F00001F
        out.append(w)
    return out


def _words(data, at, count):
    return [struct.unpack_from("<I", data, at + 4 * i)[0] for i in range(count)]


def name(kc, fix, in_kernel, apple_starts, names, ours, oname, oaddr, our_starts):
    """{Apple kernel address: name of ours} from the vectors; conflicting answers name nothing."""
    desc_apple = {a: n for a, n in names.items() if n.startswith("_vnop_") and n.endswith("_desc")}
    starts = set(apple_starts)
    kernel_pairs, kext_pairs = [], []
    for site, (target, _) in fix.items():
        if target in desc_apple and site + 8 in fix:
            function = fix[site + 8][0]
            if function in starts:
                (kernel_pairs if in_kernel(site) else kext_pairs).append((site, desc_apple[target], function))
    desc_ours = {a: n for n, a in oname.items() if n.startswith("_vnop_") and n.endswith("_desc")}
    funcs = set(our_starts)
    our_pairs = []
    for seg in ours.segs:
        if not seg["name"].startswith("__DATA") or not seg["filesize"]:
            continue
        for k in range(0, seg["filesize"] - 15, 8):
            desc, function = struct.unpack_from("<QQ", ours.d, seg["off"] + k)
            if desc in desc_ours and function in funcs:
                our_pairs.append((seg["va"] + k, desc_ours[desc], function))
    ours_vectors = _runs(our_pairs)
    answers = {}
    for vector in _runs(kernel_pairs):
        same = [v for v in ours_vectors if set(v) == set(vector)]
        if len(same) > 1:
            differ = [d for d in vector if len({v[d] for v in same}) > 1]
            agree = [sum(1 for d in differ if names.get(vector[d]) in oaddr[v[d]]) for v in same]
            same = [v for v, k in zip(same, agree) if k == max(agree) and agree.count(k) == 1 and k]
        if len(same) != 1:
            continue
        for desc, function in vector.items():
            answers.setdefault(function, set()).add(sorted(oaddr[same[0][desc]])[0])

    def body_apple(a):
        i = bisect.bisect_right(apple_starts, a)
        end = apple_starts[i] if i < len(apple_starts) else a + 4
        return _body(_words(kc.d, kc.va2off(a), (min(end, a + 256) - a) // 4))

    def body_ours(a):
        i = bisect.bisect_right(our_starts, a)
        end = our_starts[i] if i < len(our_starts) else a + 4
        return _body(_words(ours.d, ours.va2off(a), (min(end, a + 256) - a) // 4))

    for _, desc, function in kext_pairs:
        if not in_kernel(function) or function in names or function in answers:
            continue
        op = desc[len("_vnop_"):-len("_desc")]
        fits = [n for n in ("_err_" + op, "_nop_" + op) if n in oname and body_ours(oname[n]) == body_apple(function)]
        if len(fits) == 1:
            answers.setdefault(function, set()).add(fits[0])
    # Apple folds identical bodies into one function (err_advlock serves vnop_copyfile too); ours keeps both
    return {a: sorted(n)[0] for a, n in answers.items()
            if len(n) == 1 or len({tuple(body_ours(oname[x])) for x in n}) == 1}
