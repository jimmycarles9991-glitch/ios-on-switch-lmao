#!/usr/bin/env python3
import sys, collections, bisect, json, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kcinfo import KC, ROOTS, decode8
import paths
CUTS = {("com.apple.iokit.IOUSBHostFamily", "com.apple.driver.AppleSMC"),
        ("com.apple.iokit.IOUSBHostFamily", "com.apple.driver.usb.AppleUSBHostPlatformProperties"),
        ("com.apple.iokit.IOAccessoryManager", "com.apple.driver.AppleOnboardSerial")}
kc = KC(sys.argv[1])
roots = [r for r in ROOTS if r in kc.plist]
sel, ifaces, codeless = kc.closure(roots, CUTS)
print("closure with main's cuts:", len(sel))
print(sorted(sel))
ext = []
for n, vm, fo in kc.entries:
    for s in kc.entry_segs(fo):
        if s["size"] and s["name"] != "__LINKEDIT": ext.append((s["va"], s["va"] + s["size"], n, s["name"]))
ext.sort(); starts = [e[0] for e in ext]
def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return ext[i] if i >= 0 and a < ext[i][1] else None
cnt = collections.Counter(); fmts = collections.Counter(); kt = collections.Counter(); other = collections.Counter()
nfix = 0
for site, raw, fmt in kc.fixups_all():
    nfix += 1; fmts[fmt] += 1
    o = owner(site)
    if not o or o[2] not in sel: continue
    f = decode8(raw); tgt = kc.base + f["target"]
    t = owner(tgt)
    if t and t[2] == o[2]: kind = "self"
    elif t and t[2] == "com.apple.kernel": kind = "kernel"; kt[tgt] += 1
    elif t and t[2] in sel: kind = "other selected kext"
    else:
        kind = "unselected"; other[(o[2], t[2] if t else None)] += 1
    cnt[(kind, bool(f["auth"]))] += 1
print("fixups total", nfix, dict(fmts))
for k, v in sorted(cnt.items()): print("  ", k, v)
print("distinct kernel targets", len(kt))
for k, v in other.most_common(30): print("  unselected:", k, v)
json.dump({"selected": sorted(sel), "interfaces": sorted(ifaces), "codeless": sorted(codeless),
           "kernel_targets": sorted(kt)}, open(paths.work("closure.json"), "w"), indent=0)
