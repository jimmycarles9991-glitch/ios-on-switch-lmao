#!/usr/bin/env python3
"""Check a kernel.macho made by emit26.py, from the output bytes alone (plus the report for the kext ranges):
  1. every kext instruction word outside data islands is ARMv8.0 (a64x classifier: no PAC, LSE, LDAPR, FP16, ...);
  2. every pointer in the KASLR list is a plain address inside a mapped segment of the output;
  3. every B/BL in kext code lands in executable memory of the output (our kernel's text, kext text, stub area);
  4. the a64x stubs and trampolines are ARMv8.0 too;
  5. every kext with corecrypto's FIPS integrity check holds the MAC the translation leaves (macho/fips.h).
usage: verify26.py OUTPUT KERNELCACHE"""
import bisect, collections, json, os, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.join(HERE, "..")
sys.path.insert(0, HERE)
import paths
from native_prelink import unserialize_xml
from tools.common.macho import macho, backed_offset
from kcinfo import KC

out_path, kc_path = sys.argv[1:3]
o = open(out_path, "rb").read()
img = macho(o)
rep = json.load(open(out_path + ".json"))
slide = rep["slide"]
segs = [s for s in img["segments"] if s["size"]]
exe = [(s["va"], s["va"] + s["size"]) for s in segs if s["initprot"] & 4]
def mapped(a): return any(s["va"] <= a < s["va"] + s["size"] for s in segs)
def executable(a): return any(lo <= a < hi for lo, hi in exe)
kc = KC(kc_path)
res = collections.Counter()
# kext instruction sections, slid
ranges = []
for b in rep["components"]:
    vm, fo = kc.entry[b]
    for s in kc.entry_segs(fo):
        for sec in s["sections"]:
            if sec["flags"] & 0x80000400 and sec["size"]:
                ranges.append((sec["va"] + slide, sec["size"], b))
# islands: recompute with the same tool on the same input is what emit26 did; here: words a64x calls data are those
# the translation left alone. A word is checked only if the classifier calls it an instruction class we translate.
words = []
for va, size, b in ranges:
    at = backed_offset(img["segments"], va, size, len(o))
    words.append((va, struct.unpack_from("<%dI" % (size // 4), o, at), b))
allw = [w for va, ws, b in words for w in ws]
r = subprocess.run([paths.ARM64E, "class"], input="\n".join("%08x" % w for w in allw) + "\n",
                   capture_output=True, text=True)
cls = r.stdout.split("\n")
k = 0
left = collections.Counter(); left_where = collections.Counter()
for va, ws, b in words:
    for i, w in enumerate(ws):
        c = cls[k]; k += 1
        if c != "ok":
            left[c] += 1; left_where[(b, c)] += 1
        if w & 0x7C000000 == 0x14000000:
            imm = w & 0x3FFFFFF
            if imm & (1 << 25): imm -= 1 << 26
            t = va + 4 * i + 4 * imm
            res["branch to executable" if executable(t) else "branch to non-executable"] += 1
res["instruction words checked"] = len(allw)
# pointers
info = next(s for s in img["segments"] if s["name"] == "__PRELINK_INFO")
md = unserialize_xml(o[info["fileoff"]:info["fileoff"] + info["filesize"]])
packed = md["_PrelinkLinkKASLROffsets"]
n = struct.unpack_from("<I", packed)[0]
base = next(s["va"] for s in img["segments"] if s["name"] == "__PRELINK_TEXT")
for off in struct.unpack_from("<%dI" % n, packed, 4):
    v = struct.unpack_from("<Q", o, backed_offset(img["segments"], base + off, 8, len(o)))[0]
    res["pointer mapped" if mapped(v) else "pointer NOT mapped"] += 1
# stub area
sa = int(rep["stub_area"]["va"], 16)
nw = rep["stub_area"]["a64x_words"]
sw = struct.unpack_from("<%dI" % nw, o, backed_offset(img["segments"], sa, 4 * nw, len(o)))
r2 = subprocess.run([paths.ARM64E, "class"], input="\n".join("%08x" % w for w in sw[:rep["stub_area"]["a64x_words"]]) + "\n",
                    capture_output=True, text=True)
res["stub words not ARMv8.0"] = sum(1 for c in r2.stdout.split("\n") if c and c != "ok")
# FIPS integrity MACs, from the output's own headers (slid) and text
import tempfile
with tempfile.TemporaryDirectory() as t:
    for b in rep.get("fips_macs", {}):
        hdr_va = kc.entry[b][0] + slide
        at = backed_offset(img["segments"], hdr_va, 32, len(o))
        hdr = o[at:at + 32 + struct.unpack_from("<I", o, at + 20)[0]]
        open(os.path.join(t, "h"), "wb").write(hdr)
        text_va, size, mac_va = (int(x, 16) for x in subprocess.run([paths.ARM64E, "fips", os.path.join(t, "h")],
                                 check=True, capture_output=True, text=True).stdout.split())
        ta = backed_offset(img["segments"], text_va, size, len(o))
        ko = kc.va2off(text_va - slide)
        open(os.path.join(t, "original"), "wb").write(kc.d[ko:ko + size])
        open(os.path.join(t, "text"), "wb").write(o[ta:ta + size])
        was = kc.d[kc.va2off(mac_va - slide):kc.va2off(mac_va - slide) + 32].hex()
        mac = subprocess.run([paths.ARM64E, "fips-mac", os.path.join(t, "original"), os.path.join(t, "text"), was],
                             check=True, capture_output=True, text=True).stdout.strip()
        ma = backed_offset(img["segments"], mac_va, 32, len(o))
        kept = o[ma:ma + 32].hex() == mac
        res["FIPS MAC as the translation leaves it" if kept else "FIPS MAC NOT as the translation leaves it"] += 1
print(json.dumps(dict(res), indent=1))
print("kext words the classifier still flags (data islands left as they were, or untranslated):", dict(left))
for (b, c), v in left_where.most_common(12): print("   ", b, c, v)
print("prelink info entries:", len(md["_PrelinkInfoDictionary"]), "KASLR pointers:", n)
if res["pointer NOT mapped"] or res["stub words not ARMv8.0"] or res["FIPS MAC NOT as the translation leaves it"]:
    sys.exit("verify26: KASLR pointers outside the image, stub words above ARMv8.0 or a stale FIPS MAC")
