#!/usr/bin/env python3
"""Name the 26.5.2 kernel addresses that kexts use, so the kexts can be relinked against our kernel.

The 26.5.2 kernelcache is fully stripped. Sources, strongest first:
  1. vtables: Apple kernel class C, slot i (ipsw kernel cpp) <-> our kernel's __ZTV<C> slot i (same 12377 KPI).
     Checked with the arm64e vtable PAC: the slot's discriminator is the SipHash of the mangled name of the method
     that introduced the slot (prelink26/ptrauth_disc.py computes it with clang).
  2. GOT order: a kext's __auth_got / __got entries are sorted by imported symbol name (98.3% of adjacent pairs in
     the symbolized iOS 17 cache; the rest are aliases). Between two named neighbours, the unknown entries take the
     names that the same kext imported in iOS 17 in that interval, or the 26.5.2 KPI names (symbol sets) there,
     when the count matches exactly. Iterated over every kext until nothing changes.
  3. ipsw/symbolicator string signatures (Darwin 25.5): seeds for C functions, and a hold-out check of 1-2.
  4. pinned_names.py: functions identified by hand for one kernelcache, before the GOT pass; any disagreement fails.
Output: prelink26/kernel-names.json {address: name}, with per-source counts and conflicts.
"""
import bisect, collections, itertools, json, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.join(HERE, "..")
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(R, "tools"))
from kcinfo import KC, decode8
from fileset import Fileset
import kc17
import paths
import pinned_names

KCPATH = paths.KERNELCACHE
OURS = paths.OURS


def mangle_class(c):
    parts = c.split("::")
    if len(parts) == 1:
        return "%d%s" % (len(c), c)
    return "N" + "".join("%d%s" % (len(p), p) for p in parts) + "E"


def load_ours():
    k = Fileset(OURS)
    by_name, by_addr, ordered = {}, collections.defaultdict(set), []
    for l in open(paths.work("ours.nm")):
        a, t, n = l.split()
        a = int(a, 16)
        by_name.setdefault(n, a); by_addr[a].add(n); ordered.append((a, n))
    text = [(s["va"], s["va"] + s["size"]) for s in k.segs if s["initprot"] & 4]
    def is_func(a): return any(lo <= a < hi for lo, hi in text)
    return k, by_name, by_addr, sorted(set(ordered)), is_func


def main():
    kc = KC(KCPATH)
    ours, oname, oaddr, osorted, ofunc = load_ours()
    kernel_ext = [(s["va"], s["va"] + s["size"]) for s in kc.entry_segs(kc.entry["com.apple.kernel"][1]) if s["size"]]
    def in_kernel(a): return any(lo <= a < hi for lo, hi in kernel_ext)
    fix = {}
    for site, raw, fmt in kc.fixups_all():
        f = decode8(raw); fix[site] = (kc.base + f["target"], f)
    names = {}            # apple kernel address -> name
    source = {}
    conflicts = []

    def assign(a, n, src):
        if a in names:
            if names[a] != n:
                # aliases at one address of our kernel are the same function
                if oname.get(n) is not None and oname.get(n) == oname.get(names[a]):
                    return False
                conflicts.append((hex(a), names[a], source[a], n, src))
            return False
        names[a] = n; source[a] = src
        return True

    # 1. vtables, each slot checked against its arm64e PAC discriminator
    from ptrauth_disc import discriminators
    cpp = json.load(open(paths.work("cpp26.json")))
    kcls = {c["class"]: c for c in cpp if c["bundle"] == "com.apple.kernel"}
    meta_of = {}                       # our gMetaClass address -> class
    for c in kcls:
        g = oname.get("__ZN" + mangle_class(c)[0:] + "10gMetaClassE") if "::" not in c else None
        if g: meta_of[g] = c
    # OSObject and OSMetaClass derive from OSMetaClassBase, which has no metaclass: its slots come from our vtable
    zb = oname["__ZTV15OSMetaClassBase"] + 16; nb = 0
    while True:
        v = struct.unpack_from("<Q", ours.d, ours.va2off(zb + 8 * nb))[0]
        if not v or not ofunc(v): break
        nb += 1
    kcls["OSMetaClassBase"] = {"class": "OSMetaClassBase", "num_methods": nb, "methods": [], "bundle": "com.apple.kernel"}
    def superclass(c):
        if c in ("OSObject", "OSMetaClass"): return "OSMetaClassBase"
        sc = oname.get("__ZN" + mangle_class(c) + "10superClassE")
        if sc is None: return None
        v = struct.unpack_from("<Q", ours.d, ours.va2off(sc))[0]
        return meta_of.get(v)
    def our_slot(c, i):
        z = oname.get("__ZTV" + mangle_class(c))
        if z is None: return None
        v = struct.unpack_from("<Q", ours.d, ours.va2off(z + 16 + 8 * i))[0]
        ns = sorted(oaddr.get(v, ()))
        return (v, ns[0]) if ns else (v, None)
    our_count = {}
    def ocount(c):
        if c not in our_count:
            z = oname.get("__ZTV" + mangle_class(c)); k = 0
            if z is not None:
                while True:
                    v = struct.unpack_from("<Q", ours.d, ours.va2off(z + 16 + 8 * k))[0]
                    if not v or not ofunc(v): break
                    k += 1
            our_count[c] = k
        return our_count[c]
    def root_name(c, i):
        """mangled name (no Mach-O underscore) of the method that introduced our slot i of class c"""
        chain = [c]
        while True:
            sc = superclass(chain[-1])
            if sc is None or sc in chain: break
            chain.append(sc)
        intro = [a for a in chain if ocount(a) > i]
        if not intro: return None
        top = intro[-1]
        for a in reversed(intro):               # topmost first; skip pure virtual roots
            r = our_slot(a, i)
            if r and r[1] and r[1] != "___cxa_pure_virtual":
                n = r[1]
                if a != top:                    # re-root the implementer's name onto the introducing class
                    for pre in ("__ZNK", "__ZN"):
                        imp = pre + mangle_class(a)
                        if n.startswith(imp): n = pre + mangle_class(top) + n[len(imp):]; break
                    if a == "OSObject" or not n.startswith(("__ZN" + mangle_class(top), "__ZNK" + mangle_class(top))):
                        # destructors are named after their own class: D1/D0 of the root
                        if n.endswith(("D1Ev", "D0Ev")): n = "__ZN" + mangle_class(top) + n[-4:]
                return n[1:]
        return None
    vt_stats = collections.Counter()
    vt_slots = []
    per_class = {}
    for c in kcls.values():
        if c["class"] == "OSMetaClassBase": continue
        if "__ZTV" + mangle_class(c["class"]) not in oname:
            vt_stats["class missing in ours"] += 1; continue
        vt_stats["classes"] += 1
        ours_slots = []
        for j in range(ocount(c["class"])):
            r = our_slot(c["class"], j)
            ours_slots.append((j, r, root_name(c["class"], j)))
        per_class[c["class"]] = (c, ours_slots)
    disc = discriminators([x[2] for c, sl in per_class.values() for x in sl if x[2]])
    for cls, (c, sl) in per_class.items():
        by_disc = collections.defaultdict(list)
        for j, r, root in sl:
            if root and r and r[1]: by_disc[disc[root]].append((j, r))
        for m in c["methods"]:
            cand = by_disc.get(m.get("pac"), [])
            if len(cand) != 1:
                vt_stats["slot: no unique PAC match" if not cand else "slot: PAC ambiguous"] += 1; continue
            j, (t_ours, n_ours) = cand[0]
            vt_stats["slot PAC-matched" + (" (moved)" if j != m["index"] else "")] += 1
            vt_slots.append((cls, m["index"], int(m["target"], 16), t_ours, n_ours, m.get("pac")))
    # an Apple function shared by slots of different methods (__cxa_pure_virtual, reserved-slot stubs) has no
    # single counterpart: vtables name only targets whose slots all map to one function of ours
    by_target = collections.defaultdict(set)
    for cls, i, t_apple, t_ours, n_ours, pac in vt_slots:
        by_target[t_apple].add((t_ours, n_ours))
    pacs_of = collections.defaultdict(set)          # every class, kexts' included
    for c in cpp:
        for m in c["methods"]: pacs_of[int(m["target"], 16)].add(m.get("pac"))
    for t_apple, outs in by_target.items():
        if not in_kernel(t_apple): continue
        if len(set(o for o, n in outs)) == 1 and len(pacs_of[t_apple]) == 1:
            assign(t_apple, sorted(outs)[0][1], "vtable")
        else:
            vt_stats["apple target shared by different methods (not named)"] += 1
    # 3a. symbolicator: C names only (C++ ones come demangled)
    sym = json.load(open(paths.work("sym/kernelcache.decompressed.symbols.json")))
    symn = {int(a): n for a, n in sym.items()}
    holdout = {}
    for a, n in symn.items():
        if not in_kernel(a): continue
        if n.endswith("::gMetaClass") and n.count("::") == 1:
            mn = "__ZN" + mangle_class(n[:-12]) + "10gMetaClassE"
        elif n.startswith("vtable for ") and n.count("::") <= 1:
            # ipsw gives the vtable's address point; the __ZTV symbol is 16 bytes before it
            c = n[11:]
            mn = "__ZTVN" + mangle_class(c[:-11]) + "9MetaClassE" if c.endswith("::MetaClass") else \
                 ("__ZTV" + mangle_class(c) if "::" not in c else None)
            if mn is None: continue
            a = a - 16
            if not in_kernel(a): continue
        elif "::" in n or " " in n or "(" in n:
            continue
        else:
            mn = "_" + n
        if mn in oname:
            holdout[a] = mn
    # seed half of the symbolicator names, keep the other half to check the GOT pass
    seeds = sorted(a for a in holdout if not holdout[a].startswith(("__ZN", "__ZTV")))
    for a in holdout:
        if holdout[a].startswith(("__ZN", "__ZTV")): assign(a, holdout[a], "symbolicator-class")
    seed_all = bool(os.environ.get("NAMES_SEED_ALL"))
    for j, a in enumerate(seeds):
        if j % 2 == 0 or seed_all: assign(a, holdout[a], "symbolicator")
    for a, n in pinned_names.names_for(KCPATH).items():
        assign(a, n, "pinned")
        if n not in oname or oname.get(names[a]) != oname[n]:
            raise SystemExit("pinned name %s for %#x: not in our kernel, or named %s by %s" % (n, a, names[a], source[a]))
    # vnode operation descriptors (struct vnodeop_desc, vnode_internal.h) carry their own name at +8: "vnop_<op>" for
    # vnop_<op>_desc, "default" for vnop_default_desc. File systems point their operation vectors at them.
    for a in sorted(set(t for t, f in fix.values() if in_kernel(t) and t not in names)):
        target = fix.get(a + 8)
        if not target: continue
        at = kc.va2off(target[0])
        end = kc.d.find(b"\0", at, at + 64) if at is not None else -1
        if end < 0: continue
        op = kc.d[at:end].decode(errors="replace")
        n = "_vnop_default_desc" if op == "default" else "_%s_desc" % op if op.startswith("vnop_") else None
        if n and n in oname and not ofunc(oname[n]):
            assign(a, n, "vnodeop_desc")
    # 2. GOT order
    d17, kn17, names17, kexts17, gots17 = kc17.load()
    universe = set(l.strip() for l in open(paths.work("symbolsets26.txt")))
    gots26 = {}
    for bid, vm, fo in kc.entries[1:]:
        secs = []
        for sg in kc.entry_segs(fo):
            for sec in sg["sections"]:
                if sec["name"] in ("__auth_got", "__got") and sec["size"]:
                    secs.append((sec["name"], [fix.get(sec["va"] + 8 * i, (None, None))[0] for i in range(sec["size"] // 8)]))
        gots26[bid] = secs
    imports17 = {bid: sorted(set(n for t, ns in seq for n in ns)) for bid, seq in gots17.items()}
    union17 = sorted(set(n for v in imports17.values() for n in v))
    funcs_u = sorted(n for n in universe if n in oname and ofunc(oname[n]))
    data_u = sorted(n for n in universe if n in oname and not ofunc(oname[n]))
    # every kernel entry of every GOT section: address -> [(section key, position)]
    secs_k = {}
    occ = collections.defaultdict(list)
    for bid, secs in gots26.items():
        for secname, seq in secs:
            ks = [a for a in seq if a is not None and in_kernel(a)]
            key = (bid, secname); secs_k[key] = ks
            for j, a in enumerate(ks): occ[a].append((key, j))
    alias17 = {}
    for a17, ns in kn17.items():
        for n in ns: alias17.setdefault(n, set()).update(ns)
    def aliases(n):
        out = {n} | alias17.get(n, set())
        if n in oname: out |= oaddr[oname[n]]
        return out
    def bounds(lo, hi):
        """The gap (lo, hi) between two named GOT entries, as the aliases that keep them ascending: lo's highest alias
        below hi's highest, hi's lowest above that. 24B119's kexts import IOLockLock as lck_mtx_lock (one function,
        one address), so an interval bounded by the name the entry got can come out empty or reversed."""
        la, ha = aliases(lo) if lo else {""}, aliases(hi) if hi != "\x7f" else {"\x7f"}
        top = max(ha)
        below = [x for x in la if x < top]
        if not below:
            return lo, hi
        x = max(below)
        return x, min(y for y in ha if y > x)
    def neighbours(ks, j):
        lo = next((names[ks[i]] for i in range(j - 1, -1, -1) if ks[i] in names), "")
        hi = next((names[ks[i]] for i in range(j + 1, len(ks)) if ks[i] in names), "\x7f")
        return bounds(lo, hi)
    def fits(a, n):
        """some alias of n lies between some aliases of a's named neighbours, in every section that holds a"""
        na = aliases(n)
        for key, j in occ[a]:
            ks = secs_k[key]
            lo = next((aliases(names[ks[i]]) for i in range(j - 1, -1, -1) if ks[i] in names), {""})
            hi = next((aliases(names[ks[i]]) for i in range(j + 1, len(ks)) if ks[i] in names), {"\x7f"})
            if not any(min(lo) < x < max(hi) for x in na): return False
        return True
    stats = collections.Counter()
    def got_pass():
      total = 0
      while True:
        stats['got rounds'] += 1; progress = 0
        taken = set(names.values())
        for (bid, secname), ks in secs_k.items():
            imp = imports17.get(bid, [])
            kind_ok = (lambda n: n in oname and ofunc(oname[n])) if secname == "__auth_got" else \
                      (lambda n: n in oname and not ofunc(oname[n]))
            # the gap bounds use the alias that keeps the section ascending: lowest after, highest before
            known = [(j, names[a]) for j, a in enumerate(ks) if a in names]
            edges = [(-1, "")] + known + [(len(ks), "\x7f")]
            for (j0, lo), (j1, hi) in zip(edges, edges[1:]):
                lo, hi = bounds(lo, hi)
                gap = [ks[j] for j in range(j0 + 1, j1) if ks[j] not in names]
                if not gap or len(set(gap)) != len(gap): continue
                for cand_src, pool in (("got17", imp), ("got17-any", union17),
                                       ("got26-kpi", funcs_u if secname == "__auth_got" else data_u)):
                    lo_i, hi_i = bisect.bisect_right(pool, lo), bisect.bisect_left(pool, hi)
                    cands = [n for n in pool[lo_i:hi_i] if kind_ok(n) and n not in taken]
                    if len(cands) < len(gap): continue
                    if len(cands) == len(gap):
                        choice = cands if all(fits(a, n) for a, n in zip(gap, cands)) else None
                        if choice is None: stats['rejected'] += 1; continue
                    else:
                        # more names than entries: the in-order choices every other section allows; one, or none
                        if len(cands) > 14 or len(gap) > 6: continue
                        ok = [c for c in itertools.combinations(cands, len(gap))
                              if all(fits(a, n) for a, n in zip(gap, c))]
                        if len(ok) != 1: continue
                        choice = list(ok[0]); cand_src += "+order"
                    for a, n in zip(gap, choice):
                        if assign(a, n, cand_src): progress += 1; taken.add(n)
                    break
        total += progress
        if not progress: return total

    # call graph: a named Apple function and ours of the same name call the same functions in the same order,
    # apart from what DEVELOPMENT adds (asserts) and inlining; equal-length runs between agreeing calls pair up
    import difflib
    kvm, kfo = kc.entry["com.apple.kernel"]
    ktext_vm = kc.entry_segs(kfo)[0]["va"]
    fstarts = []
    for cmd, p, size in kc.entry_cmds(kfo):
        if cmd == 0x26:
            off, sz = struct.unpack_from("<II", kc.d, p + 8); q = off; a0 = ktext_vm
            while q < off + sz:
                v = 0; sh = 0
                while True:
                    byte = kc.d[q]; q += 1; v |= (byte & 0x7F) << sh; sh += 7
                    if not byte & 0x80: break
                if not v: break
                a0 += v; fstarts.append(a0)
    fstarts.sort()
    our_starts = sorted(set(x for x, n in osorted if ofunc(x)))
    def calls(data_of, lo, hi):
        out = []
        for pc in range(lo, hi, 4):
            w = data_of(pc)
            if w is None: break
            if w & 0xFC000000 == 0x94000000 or (w & 0xFC000000 == 0x14000000):
                imm = w & 0x3FFFFFF
                if imm & (1 << 25): imm -= 1 << 26
                t = pc + 4 * imm
                if w & 0xFC000000 == 0x14000000 and lo <= t < hi: continue      # branch inside the function
                out.append(t)
        return out
    def apple_word(pc):
        o = kc.va2off(pc); return struct.unpack_from("<I", kc.d, o)[0] if o is not None else None
    def our_word(pc):
        o = ours.va2off(pc); return struct.unpack_from("<I", ours.d, o)[0] if o is not None else None
    def callgraph_pass():
        votes = collections.defaultdict(collections.Counter)
        single = set()
        for a0, n in list(names.items()):
            if n not in oname or not ofunc(oname[n]): continue
            i = bisect.bisect_left(fstarts, a0)
            if i >= len(fstarts) or fstarts[i] != a0: continue
            ahi = fstarts[i + 1] if i + 1 < len(fstarts) else a0 + 4
            o0 = oname[n]; j = bisect.bisect_right(our_starts, o0)
            ohi = our_starts[j] if j < len(our_starts) else o0 + 4
            if ahi - a0 > 0x20000 or ohi - o0 > 0x40000: continue
            ta = [("K", oname[names[t]]) if t in names and names[t] in oname else ("U", t) for t in calls(apple_word, a0, ahi)]
            to = [("K", t) for t in calls(our_word, o0, ohi)]
            sm = difflib.SequenceMatcher(None, ta, to, autojunk=False)
            for tag, i1, i2, j1, j2 in sm.get_opcodes():
                if tag != "replace" or i2 - i1 != j2 - j1: continue
                for x, y in zip(ta[i1:i2], to[j1:j2]):
                    if x[0] == "U" and ofunc(y[1]) and oaddr.get(y[1]):
                        votes[x[1]][y[1]] += 1
                        if i2 - i1 == 1: single.add((x[1], y[1]))
        added = 0
        used = set(oname[n] for n in names.values() if n in oname)
        for apple, c in votes.items():
            if apple in names or not in_kernel(apple): continue
            (b0, c1), *rest = c.most_common(2) + [(None, 0)]
            c2 = rest[0][1]
            if b0 in used or c2: continue
            if c1 >= 2 or (apple, b0) in single:
                nm = sorted(oaddr[b0])[0]
                # hold-out: right for 93% of internal functions but only 5 of 8 exported ones (inlining differs most
                # around KPI wrappers), so the call graph never names a KPI symbol; it only extends the graph
                if any(x in universe for x in oaddr[b0]): continue
                # an address some kext imports has its name's place in that kext's GOT
                if apple in occ and not fits(apple, nm): continue
                if assign(apple, nm, "callgraph"): added += 1; used.add(b0)
        return added
    while True:
        g = got_pass()
        c = callgraph_pass()
        stats["callgraph named"] += c
        if not g and not c: break
    rejected = stats["rejected"]; rounds = stats["got rounds"]
    print("call graph named", stats["callgraph named"])
    # interval pass: an unknown address takes the only KPI name of its kind that fits between its named
    # neighbours in every section holding it (intersection of all its intervals)
    kind_of = {}
    for (bid, secname), ks in secs_k.items():
        for a in ks: kind_of.setdefault(a, set()).add(secname)
    all17 = sorted(set(n for v in imports17.values() for n in v) | universe)
    func_pool = [n for n in all17 if n in oname and ofunc(oname[n])]
    data_pool = [n for n in all17 if n in oname and not ofunc(oname[n])]
    ipass = 0
    while True:
        progress = 0
        taken = set(names.values())
        for a in list(occ):
            if a in names or len(kind_of[a]) != 1: continue
            L, H = "", "\x7f"
            for key, j in occ[a]:
                ks = secs_k[key]
                lo, hi = neighbours(ks, j)
                L, H = max(L, lo), min(H, hi)
            pool = func_pool if "__auth_got" in kind_of[a] else data_pool
            c = [n for n in pool[bisect.bisect_right(pool, L):bisect.bisect_left(pool, H)] if n not in taken]
            if len(c) == 1 and fits(a, c[0]):
                if assign(a, c[0], "got-interval"): progress += 1; taken.add(c[0]); ipass += 1
        if not progress: break
    print("interval pass named", ipass)
    if os.environ.get("NAMES_DEBUG"):
        cl0 = json.load(open(paths.work("closure.json")))
        need0 = set(cl0["kernel_targets"]) | set(cl0["kernel_branch_targets"])
        shown = 0
        for a in sorted(need0):
            if a in names: continue
            if a not in occ: print("  unnamed, in no GOT:", hex(a)); shown += 1; continue
            L, H = "", "\x7f"; nb = []
            for key, j in occ[a]:
                ks = secs_k[key]
                lo, hi = neighbours(ks, j)
                nb.append((key[0].split(".")[-1], key[1], lo, hi))
                L, H = max(L, lo), min(H, hi)
            pool = func_pool if "__auth_got" in kind_of[a] else data_pool
            c = [n for n in pool[bisect.bisect_right(pool, L):bisect.bisect_left(pool, H)] if n not in set(names.values())]
            print("  unnamed", hex(a), kind_of[a], "occ", len(occ[a]), "interval", L, H, "cands", len(c), c[:6])
            shown += 1
            if shown > 12: break
    # audit: every section's named entries must ascend
    viol = 0
    for key, ks in secs_k.items():
        kn = [names[a] for a in ks if a in names]
        viol += sum(1 for x, y in zip(kn, kn[1:]) if not min(aliases(x)) < max(aliases(y)))
    print("GOT gap fills rejected by another section's order:", rejected, "order violations after:", viol)
    # hold-out check
    chk = collections.Counter()
    for j, a in enumerate(seeds):
        if j % 2 == 1 and not seed_all:
            if a in names:
                ok = names[a] == holdout[a] or oname.get(names[a]) == oname.get(holdout[a])
                chk["agree" if ok else "disagree"] += 1
                kpi = names[a] in universe
                chk[("KPI " if kpi else "non-KPI ") + source[a] + (" agree" if ok else " disagree")] += 1
                if not ok and chk["disagree"] <= 10: print("  holdout disagree", hex(a), names[a], source[a], "vs", holdout[a])
            else:
                chk["not named by vtable/GOT"] += 1
    # the targets the kept kexts need
    cl = json.load(open(paths.work("closure.json")))
    need = sorted(set(cl["kernel_targets"]) | set(cl["kernel_branch_targets"]))
    got = [a for a in need if a in names]
    print("vtables:", dict(vt_stats), "slots", len(vt_slots))
    print("GOT rounds", rounds, "names total", len(names), collections.Counter(source.values()))
    print("hold-out (symbolicator names not seeded):", dict(chk))
    print("conflicts", len(conflicts)); [print("  ", c) for c in conflicts[:10]]
    print("kept kexts need", len(need), "named", len(got), "by source", collections.Counter(source[a] for a in got))
    missing = [a for a in need if a not in names]
    json.dump({"names": {hex(a): n for a, n in names.items()}, "source": {hex(a): s for a, s in source.items()},
               "missing": [hex(a) for a in missing], "conflicts": conflicts, "vt_slots": vt_slots,
               "holdout": dict(chk)},
              open(paths.work("kernel-names.json" if seed_all else "kernel-names-holdout.json"), "w"), indent=0)


if __name__ == "__main__":
    main()
