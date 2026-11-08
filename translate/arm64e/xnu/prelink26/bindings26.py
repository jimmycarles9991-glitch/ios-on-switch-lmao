#!/usr/bin/env python3
"""Our drivers' original-bindings for the 26.5.2 kernelcache (round 6, task 2).

The main tree pins each binding to a hand-audited 21H461 address (abi_check.py --write, build-native-*.py); 375 of
the 510 are vtable slots our subclasses inherit ("opaque<slot>"). 26.5.2 is a stripped arm64e fileset, so each one
is found again from the 26.5.2 bytes:
  class      gMetaClass / __ZTV (16 bytes before the address point) / metaclass vtable: ipsw's C++ discovery
  slot       the 21H461 slot index of the symbol (its address in the 21H461 vtable, or the opaque<slot> name),
             mapped to the 26.5.2 slot through anchors: slots named in both (26.5.2 by PAC discriminator =
             ptrauth_string_discriminator(real mangled root) for the names in our kernel and the 25.5 signatures;
             21H461 by our bindings' own names). Between two anchors equal-length runs pair in order; a run that
             grew or shrank is left unresolved (a new or removed virtual there: the driver's class must change).
  other      25.5 signatures (exact mangled name), for non-virtual members
Writes prelink26/bindings/<file> and prelink26/bindings26.json.
"""
import bisect, collections, json, os, re, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ptrauth_disc import discriminators
from tools.common.macho import macho, backed_offset

M64 = (1 << 64) - 1
SLIDE17 = -0x1000000
rows = json.load(open(os.path.join(HERE, "bindings17.json")))
cpp = {c["class"]: c for c in json.load(open(os.path.join(HERE, "cpp26.json")))}
slide = json.load(open(os.path.join(HERE, "out/kernel.macho.json")))["slide"]
inh, by_meta = {}, {}
for l in open(os.path.join(HERE, "inherit26.txt")):
    m = re.match(r"init=(\S+) size=(\S+) parent=(\S+) meta=(\S+) metavtab=(\S+) vtab=(\S+) (\S+)\s+\((\S+)\)", l)
    if m:
        inh[m.group(7)] = dict(meta=int(m.group(4), 16), parent=int(m.group(3), 16), vtab=int(m.group(6), 16),
                               metavtab=int(m.group(5), 16))
        by_meta[int(m.group(4), 16)] = m.group(7)
def chain(c):
    out = [c]
    while True:
        p = by_meta.get(inh.get(out[-1], {}).get("parent"))
        if p is None or p in out: break
        out.append(p)
    return out + (["OSMetaClassBase"] if out[-1] in ("OSObject", "OSMetaClass") else [])
def class_of(s):
    m = re.match(r"__ZN(K?)(\d+)", s)
    if not m: return None
    k = int(m.group(2)); return s[m.end():m.end() + k]
def demangle(names):
    r = subprocess.run(["xcrun", "c++filt"], input="\n".join(n[1:] if n.startswith("__") else n for n in names) + "\n",
                       capture_output=True, text=True)
    return dict(zip(names, r.stdout.split("\n")))
def method_key(dm):
    """'Class::method(args) const' -> ('method', nargs)"""
    m = re.match(r"(?:.*::)?([~\w]+)\((.*)\)", dm)
    if not m: return None
    args = m.group(2).strip()
    n = 0 if args in ("", "void") else args.count(",") + 1
    return (m.group(1), n)

# --- name the 26.5.2 slots of the classes we bind, by PAC -------------------------------------------------------------
pool = set()
for l in open(os.path.join(HERE, "ours.nm")):
    n = l.split()[2]
    if n.startswith("__ZN"): pool.add(n)
sym = collections.defaultdict(set)
for a, n in json.load(open(os.path.join(HERE, "sym2/kernelcache.decompressed.symbols.json"))).items():
    sym[n].add(int(a))
    if n.startswith("__ZN") and ".cold" not in n: pool.add(n)
for r in rows:
    if r["symbol"].startswith("__ZN") and "opaque" not in r["symbol"]: pool.add(r["symbol"])
pool = sorted(pool)
disc = discriminators([p[1:] for p in pool])
by_disc = collections.defaultdict(list)
for p in pool: by_disc[disc[p[1:]]].append(p)
classes = sorted(set(c for c in (class_of(r["symbol"]) for r in rows) if c in cpp))
slot_name26 = {}
for c in classes:
    anc = set(chain(c))
    for m in cpp[c]["methods"]:
        cands = [p for p in by_disc.get(m.get("pac"), []) if class_of(p) in anc]
        if len(cands) == 1: slot_name26[(c, m["index"])] = cands[0]
dm = demangle(sorted(set(slot_name26.values()) | set(r["symbol"] for r in rows)))
# --- 21H461 vtables -----------------------------------------------------------------------------------------------
d17 = open(os.path.join(HERE, "../../../../vendor/unixnu/out/ipsw/21H461/kernelcache.decompressed"), "rb").read()
k17 = macho(d17); s17 = k17["segments"]
exe17 = [(s["va"], s["va"] + s["size"]) for s in s17 if s["initprot"] & 4]
vt17 = {}
for r in rows:
    m = re.match(r"__ZTV(\d+)(\w+)$", r["symbol"])
    if m: vt17[m.group(2)] = int(r["addr17"], 16)
vt17.setdefault("IOUSBDeviceController", 0xfffffff006e81d90)      # abi_check.py VTABLE (not in a bindings file)
slots17 = {}
for c, z in vt17.items():
    out = []
    while True:
        v = struct.unpack_from("<Q", d17, backed_offset(s17, z + 16 + 8 * len(out), 8, len(d17)))[0]
        if not any(lo <= v < hi for lo, hi in exe17): break
        out.append(v)
    slots17[c] = out
# --- 17 slot -> 26 slot, per class -----------------------------------------------------------------------------------
maps, layout = {}, {}
for c in classes:
    if c not in slots17 or not slots17[c]: continue
    s17_ = slots17[c]; n26 = cpp[c]["num_methods"]
    idx_of_addr = collections.defaultdict(list)
    for i, v in enumerate(s17_): idx_of_addr[v].append(i)
    anchors = {}
    for r in rows:
        if class_of(r["symbol"]) != c or "opaque" in r["symbol"]: continue
        ii = idx_of_addr.get(int(r["addr17"], 16), [])
        key = method_key(dm.get(r["symbol"], ""))
        js = [j for (cc, j), p in slot_name26.items() if cc == c and method_key(dm.get(p, "")) == key]
        if len(ii) == 1 and len(js) == 1: anchors[ii[0]] = js[0]
    # the inherited kernel prefix is the same in both (the kernel KPI): slot i == slot i up to the parent's size
    par = chain(c)[1] if len(chain(c)) > 1 else None
    base = cpp[par]["num_methods"] if par in cpp else 0
    for i in range(min(base, len(s17_))): anchors.setdefault(i, i)
    pts = sorted(anchors.items()) + [(len(s17_), n26)]
    m = {}; prev = (-1, -1); bad = 0
    for i, j in pts:
        di, dj = i - prev[0], j - prev[1]
        if di == dj and di > 0:
            for k in range(1, di): m[prev[0] + k] = prev[1] + k
        elif di > 0:
            bad += di - 1
        if i < len(s17_): m[i] = j
        prev = (i, j)
    maps[c] = m
    layout[c] = {"slots_21H461": len(s17_), "slots_26.5.2": n26, "anchors": len(anchors), "unmapped_21H461_slots": bad}
# --- resolve -------------------------------------------------------------------------------------------------------
res = {}
for i, r in enumerate(rows):
    s = r["symbol"]; c = class_of(s)
    mm = re.match(r"__ZN(\d+)(\w+?)10gMetaClassE$", s)
    if mm and mm.group(2) in inh: res[i] = (inh[mm.group(2)]["meta"], "class: gMetaClass"); continue
    mm = re.match(r"__ZTV(\d+)(\w+)$", s)
    if mm and mm.group(2) in inh: res[i] = (inh[mm.group(2)]["vtab"] - 16, "class: vtable"); continue
    if c in maps:
        op = re.search(r"9opaque(\d+)Ev$", s)
        if op: i17 = int(op.group(1))
        else:
            ii = [k for k, v in enumerate(slots17[c]) if v == int(r["addr17"], 16)]
            i17 = ii[0] if len(ii) == 1 else None
        if i17 is not None and i17 in maps[c]:
            j = maps[c][i17]
            res[i] = (int(cpp[c]["methods"][j]["target"], 16), "slot: %s 21H461 #%d -> 26.5.2 #%d" % (c, i17, j)); continue
    # a virtual whose own mangled name (re-rooted on an ancestor) is the slot's PAC root
    if c in cpp and "opaque" not in s:
        const = s.startswith("__ZNK"); rest = s[(5 if const else 4) + len(str(len(c))) + len(c):]
        roots = [("_ZNK" if const else "_ZN") + "%d%s" % (len(a), a) + rest for a in chain(c)]
        dd = discriminators(roots)
        hit = [(rt, m) for rt in roots for m in cpp[c]["methods"] if m.get("pac") == dd[rt]]
        if len(hit) == 1:
            res[i] = (int(hit[0][1]["target"], 16), "slot: %s #%d by PAC of %s" % (c, hit[0][1]["index"], hit[0][0])); continue
    a = sym.get(s) or sym.get(s[1:])
    if a and len(a) == 1: res[i] = (next(iter(a)), "signature (symbolicator 25.5)"); continue
    # non-virtual member named by its 25.5 signature under the real (Apple) mangling
    key = method_key(dm.get(s, ""))
    if key and c:
        cand = [n for n in sym if n.startswith("__ZN") and class_of(n) == c and ".cold" not in n]
        dd = demangle(cand) if cand else {}
        hits = [n for n in cand if method_key(dd.get(n, "")) == key]
        if len(hits) == 1 and len(sym[hits[0]]) == 1:
            res[i] = (next(iter(sym[hits[0]])), "signature by name/arity (%s)" % hits[0])
# --- write -----------------------------------------------------------------------------------------------------------
out_dir = os.path.join(HERE, "bindings"); os.makedirs(out_dir, exist_ok=True)
for f in os.listdir(out_dir): os.remove(os.path.join(out_dir, f))
by_file = collections.defaultdict(list)
for i, r in enumerate(rows): by_file[r["file"]].append(i)
summary = {}
present = set(c["bundle"] for c in cpp.values())
for f, idx in by_file.items():
    lines = ["// Generated by prelink26/bindings26.py against the 26.5.2 kernelcache (xnu-12377.122.8, t8020 family): 26.5.2",
             "// addresses plus prelink26/emit26.py's slide (%#x). Lines marked UNRESOLVED must be solved before linking." % (slide & M64)]
    ok = 0
    for i in idx:
        r = rows[i]
        if i in res:
            a, how = res[i]
            lines += [".globl %s" % r["symbol"], ".set %s, %#x  // %s" % (r["symbol"], (a + slide) & M64, how)]; ok += 1
        else:
            c = class_of(r["symbol"])
            in_vt = c in slots17 and ("opaque" in r["symbol"] or int(r["addr17"], 16) in slots17[c])
            why = ("%s is not in the 26.5.2 kernelcache" % r["kext"]) if r["kext"] not in present else \
                  ("vtable slot in a run that changed size between 21H461 and 26.5.2" if in_vt else
                   "non-virtual member (or data): no 25.5 signature names it")
            lines.append("// UNRESOLVED %s (21H461 %s, %s): %s" % (r["symbol"], r["addr17"], r["kext"], why))
    open(os.path.join(out_dir, f.replace("/", "__")), "w").write("\n".join(lines) + "\n")
    summary[f] = {"symbols": len(idx), "resolved": ok}
kinds = collections.Counter(res[i][1].split(":")[0].split(" (")[0] for i in res)
json.dump({"summary": summary, "kinds": kinds, "layout": layout, "named_26_slots": len(slot_name26),
           "resolved": {rows[i]["symbol"]: [hex(res[i][0]), res[i][1]] for i in res}},
          open(os.path.join(HERE, "bindings26.json"), "w"), indent=1)
print("26.5.2 slots named by PAC:", len(slot_name26))
for c, l in layout.items(): print("  layout", c, l)
print("resolved %d of %d" % (len(res), len(rows)), dict(kinds))
for f, s in summary.items(): print("  %-55s %3d / %3d" % (f, s["resolved"], s["symbols"]))
print("unresolved by kext:", collections.Counter(rows[i]["kext"] for i in range(len(rows)) if i not in res).most_common())
