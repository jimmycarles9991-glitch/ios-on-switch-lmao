#!/usr/bin/env python3
"""The symbolized iOS 17 (21H461) prelinked kernelcache: kernel exports, kext symbols, kext GOT import names."""
import sys, struct, os, json
from native_prelink import unserialize_xml
from tools.common.macho import macho, backed_offset
import paths
PATH = paths.KERNELCACHE_17

def symtab(d, image, all_defined=True):
    out = {}
    for command, offset, length in image["commands"]:
        if command != 2: continue
        symoff, count, stroff, strsize = struct.unpack_from("<4I", d, offset + 8)
        for i in range(count):
            strx, kind, sect, desc, addr = struct.unpack_from("<IBBHQ", d, symoff + 16 * i)
            if kind & 0xe0 or (kind & 0x0e) != 0x0e: continue   # defined in a section, no stabs
            if not all_defined and not kind & 1: continue
            e = d.find(b"\0", stroff + strx)
            out.setdefault(addr, set()).add(d[stroff + strx:e].decode(errors="replace"))
    return out

def load():
    d = open(PATH, "rb").read()
    k = macho(d); parent = k["segments"]
    names = symtab(d, k)                         # kernel: addr -> names
    kernel_names = dict(names)
    info = next(s for s in parent if s["name"] == "__PRELINK_INFO")
    md = unserialize_xml(d[backed_offset(parent, info["va"], info["filesize"], len(d)):][:info["filesize"]])
    kexts = {}
    for e in md["_PrelinkInfoDictionary"]:
        va = e.get("_PrelinkExecutableLoadAddr")
        if not va or not e.get("_PrelinkExecutableSize"): continue
        try: m = macho(d, backed_offset(parent, va, 32, len(d)))
        except Exception: continue
        # kext symtab offsets in an old-style prelinked cache are file offsets of the whole cache
        try: ks = symtab(d, m)
        except Exception: ks = {}
        kexts[e["CFBundleIdentifier"]] = (m, ks)
        for a, n in ks.items(): names.setdefault(a, set()).update(n)
    gots = {}
    for bid, (m, ks) in kexts.items():
        seq = []
        for sg in m["segments"]:
            for sec in sg["sections"]:
                if sec["name"] in ("__got", "__auth_got") and sec["size"]:
                    at = backed_offset(parent, sec["va"], sec["size"], len(d))
                    for i in range(sec["size"] // 8):
                        t = struct.unpack_from("<Q", d, at + 8 * i)[0]
                        seq.append((t, sorted(names.get(t, ()))))
        gots[bid] = seq
    return d, kernel_names, names, kexts, gots

if __name__ == "__main__":
    d, kn, names, kexts, gots = load()
    tot = sum(len(v) for v in gots.values()); named = sum(1 for v in gots.values() for t, n in v if n)
    print("kernel named addrs", len(kn), "kexts", len(kexts), "GOT entries", tot, "named", named)
    print(gots.get("com.apple.iokit.IOUSBDeviceFamily", [])[:12])
