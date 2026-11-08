#!/usr/bin/env python3
"""Are kext GOT entries sorted by imported symbol name? Checked on the symbolized iOS 17 (21H461) kernelcache."""
import os, sys, struct
from native_prelink import unserialize_xml
from tools.common.macho import macho, exports, backed_offset
d = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../../vendor/unixnu/out/ipsw/21H461/kernelcache.decompressed"), "rb").read()
k = macho(d); ex = exports(d, k)
names = {a: sorted(v)[0] for a, v in ex.items()}
parent = k["segments"]
info = next(s for s in parent if s["name"] == "__PRELINK_INFO")
md = unserialize_xml(d[backed_offset(parent, info["va"], info["filesize"], len(d)):][:info["filesize"]])
tot = asc = nk = fail = 0; bad = []
for e in md["_PrelinkInfoDictionary"]:
    va = e.get("_PrelinkExecutableLoadAddr")
    if not va or not e.get("_PrelinkExecutableSize"): continue
    try: m = macho(d, backed_offset(parent, va, 32, len(d)))
    except Exception: fail += 1; continue
    for sg in m["segments"]:
        for sec in sg["sections"]:
            if sec["name"] in ("__got", "__auth_got") and sec["size"]:
                at = backed_offset(parent, sec["va"], sec["size"], len(d))
                seq = [names.get(struct.unpack_from("<Q", d, at + 8 * i)[0]) for i in range(sec["size"] // 8)]
                kn = [x for x in seq if x]; nk += 1
                for a, b in zip(kn, kn[1:]):
                    tot += 1; asc += a < b
                    if a >= b and len(bad) < 8: bad.append((e["CFBundleIdentifier"], a, b))
print("kernel exports", len(names), "kexts unparsed", fail, "GOT sections", nk, "adjacent kernel-named pairs", tot, "ascending", asc)
for b in bad: print(" ", b)
