#!/usr/bin/env python3
"""Name the fileset kernelcache's kernel addresses that kexts use, so the kexts can be relinked against our kernel.

The kernelcache is fully stripped. Sources, strongest first:
  1. vtables: Apple kernel class C, slot i (ipsw kernel cpp) <-> our kernel's __ZTV<C> slot i (same 12377 KPI).
     Checked with the arm64e vtable PAC: the slot's discriminator is the SipHash of the mangled name of the method
     that introduced the slot (prelink26/ptrauth_disc.py computes it with clang).
  2. vnode operation descriptors (their own name string) and vectors (vnodeops.py).
  3. GOT order: a kext's __auth_got / __got entries are sorted by imported symbol name (98.3% of adjacent pairs in
     the symbolized iOS 17 cache; the rest are aliases). Between two named neighbours, the unknown entries take the
     names that the same kext imported in iOS 17 in that interval when the count matches exactly. Iterated over every
     kext until nothing changes, with the call graph between named functions.
  4. ipsw's signatures of this Darwin version, where every GOT section holding the address allows the name.
  5. pinned_names.py: functions identified by hand for one kernelcache, under the same GOT check. A pin that a
     source above already names is redundant (same function) or a conflict; the prelink refuses conflicts.
  6. shims.py: imports our kernel answers with a function of its own, where nothing above names the address.
Output: prelink26/kernel-names.json {address: name}, with per-source counts and conflicts.
"""
import bisect, collections, itertools, json, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.join(HERE, "..")
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(R, "tools"))
from kcinfo import KC, decode8
from fileset import Fileset
import funcnames
import kc17
import paths
import pinned_names
import shims
import sysctls
import vnodeops

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
    # 3. ipsw: the C++ classes' metaclass and vtable symbols it finds, and its signature names (strings, callers) of
    # this Darwin version's functions. A signature can land on the wrong function (vfs_addtrigger on lck_rw_free,
    # cpu_boot_thread on ml_processor_register), so its names wait for the GOT order to check them.
    sym = json.load(open(paths.work("sym/kernelcache.decompressed.symbols.json")))
    signatures = {}
    for a, n in ((int(a), n) for a, n in sym.items()):
        if not in_kernel(a): continue
        if n.endswith("::gMetaClass") and n.count("::") == 1:
            mn = "__ZN" + mangle_class(n[:-12]) + "10gMetaClassE"
            if mn in oname: assign(a, mn, "symbolicator-class")
        elif n.startswith("vtable for ") and n.count("::") <= 1:
            # ipsw gives the vtable's address point; the __ZTV symbol is 16 bytes before it
            c = n[11:]
            mn = "__ZTVN" + mangle_class(c[:-11]) + "9MetaClassE" if c.endswith("::MetaClass") else \
                 ("__ZTV" + mangle_class(c) if "::" not in c else None)
            if mn in oname and in_kernel(a - 16): assign(a - 16, mn, "symbolicator-class")
        elif "::" not in n and " " not in n and "(" not in n:
            mn = n if n.startswith("__Z") else "_" + n
            if mn in oname: signatures[a] = mn
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
    fset = set(fstarts)
    our_starts = sorted(set(x for x, n in osorted if ofunc(x)))
    for a, n in vnodeops.name(kc, fix, in_kernel, fstarts, names, ours, oname, oaddr, our_starts).items():
        assign(a, n, "vnodeop vector")
    sysctl_names = sysctls.name(kc, fix, in_kernel, fset.__contains__)
    for a, n in sysctl_names.items():
        if n.endswith("_children") and n in oname and not ofunc(oname[n]): assign(a, n, "sysctl tree")
        elif n.startswith("_sysctl_handle_") and n in oname and a in fset: assign(a, n, "sysctl handler")
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
    def apple_size(a):
        i = bisect.bisect_right(fstarts, a)
        return (fstarts[i] if i < len(fstarts) else a + 4) - a
    def wrapped(n):
        """the function of ours that our n only sets arguments for and branches to (advisory_read: advisory_read_ext)"""
        at = ours.va2off(oname[n])
        for i in range(8):
            w = struct.unpack_from("<I", ours.d, at + 4 * i)[0]
            if w & 0xFC000000 == 0x14000000:
                imm = w & 0x3FFFFFF
                return oname[n] + 4 * i + 4 * (imm - (1 << 26) if imm & (1 << 25) else imm)
            if w & 0xFC000000 == 0x94000000 or w & 0xFFFFFC1F == 0xD65F0000:
                return None
        return None
    def our_size(n):
        i = bisect.bisect_right(our_starts, oname[n])
        return (our_starts[i] if i < len(our_starts) else oname[n] + 4) - oname[n]
    def twins(addresses):
        """Apple functions one function of ours stands for: each only branches on (kfree_data, kfree_data_external)"""
        def branch_only(a):
            if a not in fset or apple_size(a) > 0x20: return False
            words = [struct.unpack_from("<I", kc.d, kc.va2off(a) + 4 * i)[0] for i in range(apple_size(a) // 4)]
            words = [w for w in words if w not in (0xD503237F, 0xD503233F, 0xD503245F, 0xD503201F)]
            return bool(words) and words[-1] & 0xFC000000 == 0x14000000
        return all(branch_only(a) for a in addresses)
    def plausible(a, n):
        # A few instructions of ours are not a function Apple built twenty times larger: the 21H461 import that
        # fits a gap can be OutputHost64 where 24B119's kext imports withSpecification.
        if a in fset and ofunc(oname[n]) and our_size(n) <= 0x20 and apple_size(a) > 0x80 and not wrapped(n):
            return False
        # When a wrapper of ours and the function it wraps both fit a GOT gap, the order cannot tell them apart;
        # Apple's function there is the wrapper only if it is one too (24B119's apfs imports advisory_read_ext where
        # 21H461's imported advisory_read).
        m = wrapped(n) if a in fset else None
        return not (m and apple_size(a) > 0x40 and any(fits(a, x) for x in oaddr.get(m, ())))
    def got_pass():
      total = 0
      while True:
        stats['got rounds'] += 1; progress = 0
        taken = set(names.values())
        for (bid, secname), ks in secs_k.items():
            imp = imports17.get(bid, [])
            # the gap bounds use the alias that keeps the section ascending: lowest after, highest before
            known = [(j, names[a]) for j, a in enumerate(ks) if a in names]
            edges = [(-1, "")] + known + [(len(ks), "\x7f")]
            for (j0, lo), (j1, hi) in zip(edges, edges[1:]):
                lo, hi = bounds(lo, hi)
                gap = [ks[j] for j in range(j0 + 1, j1) if ks[j] not in names]
                if not gap or len(set(gap)) != len(gap): continue
                # a function pointer kexts take as data sits in __got: the kind is the target's, not the section's
                functions = {a in fset for a in gap}
                if len(functions) != 1: continue
                kind_ok = (lambda n: n in oname and ofunc(oname[n])) if functions == {True} else \
                          (lambda n: n in oname and not ofunc(oname[n]))
                # Only the kext's own iOS 17 imports: any kext's, or any KPI name, fit a gap's count by chance
                # and shifted names by one (amfi_interface_register's on libCoreEntitlements' registration,
                # proc_is64bit_data's on proc_get_syscall_filter_mask_size); pinned_names.py names the rest.
                for cand_src, pool in (("got17", imp),):
                    lo_i, hi_i = bisect.bisect_right(pool, lo), bisect.bisect_left(pool, hi)
                    cands = [n for n in pool[lo_i:hi_i] if kind_ok(n) and n not in taken]
                    if len(cands) < len(gap): continue
                    # Names one function of ours carries (ubc_create_upl, ubc_create_upl_external) may be one GOT
                    # entry or two in this kernelcache, so the count says nothing: filling by it shifted apfs's ubc_
                    # names by one.
                    # Unless every name of each such function is here: then Apple has one function per name
                    # (kfree_data, kfree_data_external).
                    if len({oname[n] for n in cands}) != len(cands) and len(cands) == len(gap):
                        complete = all(oaddr[oname[n]] & set(pool) <= set(cands) for n in cands)
                        by_function = collections.defaultdict(list)
                        for a, n in zip(gap, cands): by_function[oname[n]].append(a)
                        if not complete or not all(twins(t) for t in by_function.values() if len(t) > 1):
                            stats["alias-ambiguous"] += 1; continue
                    elif len({oname[n] for n in cands}) != len(cands): stats["alias-ambiguous"] += 1; continue
                    if len(cands) == len(gap):
                        choice = cands if all(fits(a, n) and plausible(a, n) for a, n in zip(gap, cands)) else None
                        if choice is None: stats['rejected'] += 1; continue
                    else:
                        # more names than entries: the in-order choices every other section allows; one, or none
                        if len(cands) > 14 or len(gap) > 6: continue
                        ok = [c for c in itertools.combinations(cands, len(gap))
                              if all(fits(a, n) and plausible(a, n) for a, n in zip(gap, c))]
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
    def derive():
        while True:
            g = got_pass()
            c = callgraph_pass()
            stats["callgraph named"] += c
            # spec and fifo vectors share their descs: the names derived so far tell them apart
            v = sum(assign(a, n, "vnodeop vector") for a, n in
                    vnodeops.name(kc, fix, in_kernel, fstarts, names, ours, oname, oaddr, our_starts).items())
            if not g and not c and not v: return
    derive()
    # A string that is one function's name, referenced by that function alone (its own __func__; an inlined
    # callee's __func__ turns up in every caller too)
    bound = {oname[n] for n in names.values() if n in oname}
    users = collections.Counter(t for f in fstarts if in_kernel(f) for t in funcnames.strings(kc, fstarts, f))
    for a in sorted(set(occ) - set(names)):
        if a not in fset: continue
        own = [m for m in ("_" + t for t in funcnames.strings(kc, fstarts, a) if users[t] == 1)
               if m in oname and ofunc(oname[m]) and oname[m] not in bound and fits(a, m)]
        if len(own) == 1: assign(a, own[0], "function name string")
    # a signature name or a pin stands only where the GOT order of every section holding the address allows it, and
    # never against a derived name
    needed = json.load(open(paths.work("closure.json")))
    needed = set(needed["kernel_targets"]) | set(needed["kernel_branch_targets"])
    for a, n in sysctl_names.items():
        if n.endswith("_children") or n.startswith("_sysctl_handle_"): continue
        if a in needed and a not in names and n in oname and not ofunc(oname[n]) and (a not in occ or fits(a, n)):
            assign(a, n, "sysctl variable")
    rejected_signatures = []
    for a, n in sorted(signatures.items()):
        if a in names:
            if oname.get(names[a]) != oname[n]: rejected_signatures.append((hex(a), n, names[a], source[a]))
        elif a in occ and not fits(a, n):
            rejected_signatures.append((hex(a), n, "outside its GOT interval", ""))
        else:
            assign(a, n, "signature")
    pins_redundant, pin_conflicts = [], []
    for a, n in pinned_names.names_for(KCPATH).items():
        if n not in oname:
            pin_conflicts.append((hex(a), n, "not in our kernel", ""))
        elif a in names:
            (pins_redundant if oname.get(names[a]) == oname[n] else pin_conflicts).append((hex(a), n, names[a], source[a]))
        elif a in occ and not fits(a, n):
            pin_conflicts.append((hex(a), n, "outside its GOT interval", ""))
        else:
            assign(a, n, "pinned")
    derive()
    # A gap filled while its neighbours were still unnamed can turn out to break an order named later (current_act
    # where sandbox's GOT holds the entry between current_persona_get and current_task): retract it and derive again.
    retracted = []
    while True:
        wrong = [a for a in occ if a in names and source[a].startswith("got17") and not fits(a, names[a])]
        if not wrong: break
        for a in wrong:
            retracted.append((hex(a), names.pop(a), source.pop(a)))
        derive()
    rejected = stats["rejected"]; rounds = stats["got rounds"]
    print("call graph named", stats["callgraph named"])
    cl = json.load(open(paths.work("closure.json")))
    need = sorted(set(cl["kernel_targets"]) | set(cl["kernel_branch_targets"]))
    order_violations = [(hex(a), names[a], source[a]) for a in need if a in names and a in occ and not fits(a, names[a])]
    # after the GOT-order audit: a shim's name is ours, not the one the kexts imported
    shim_conflicts = []
    for a, (n, why) in shims.shims_for(KCPATH).items():
        if a in names or n not in oname:
            shim_conflicts.append((hex(a), n, names.get(a, "not in our kernel"), source.get(a, "")))
        else:
            assign(a, n, "shim")
    viol = 0
    for key, ks in secs_k.items():
        kn = [names[a] for a in ks if a in names]
        viol += sum(1 for x, y in zip(kn, kn[1:]) if not min(aliases(x)) < max(aliases(y)))
    print("GOT gap fills rejected by another section's order:", rejected, "order violations after:", viol)
    got = [a for a in need if a in names]
    print("vtables:", dict(vt_stats), "slots", len(vt_slots))
    print("GOT rounds", rounds, "names total", len(names), collections.Counter(source.values()))
    print("conflicts", len(conflicts)); [print("  ", c) for c in conflicts[:10]]
    print("signatures rejected", len(rejected_signatures), "pins redundant", len(pins_redundant),
          "pin conflicts", len(pin_conflicts), "needed names out of GOT order", len(order_violations),
          "retracted", len(retracted), "shim conflicts", len(shim_conflicts))
    print("kept kexts need", len(need), "named", len(got), "by source", collections.Counter(source[a] for a in got))
    missing = [a for a in need if a not in names]
    json.dump({"names": {hex(a): n for a, n in names.items()}, "source": {hex(a): s for a, s in source.items()},
               "missing": [hex(a) for a in missing], "conflicts": conflicts, "vt_slots": vt_slots,
               "rejected_signatures": rejected_signatures, "pins_redundant": pins_redundant,
               "pin_conflicts": pin_conflicts, "order_violations": order_violations, "retracted": retracted,
               "shim_conflicts": shim_conflicts},
              open(paths.work("kernel-names.json"), "w"), indent=0)
    if pin_conflicts or order_violations or shim_conflicts:
        raise SystemExit("names.py: a pin or shim against what the kernelcache shows, or a name out of GOT order:\n" +
                         "\n".join(map(str, pin_conflicts + order_violations + shim_conflicts)))

if __name__ == "__main__":
    main()
