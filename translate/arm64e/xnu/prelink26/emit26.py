#!/usr/bin/env python3
"""Prelink the 26.5.2 kernelcache's kexts that we keep into our 12377 kernel (round 6).

Same output format and kernel ABI as the main tree's kits/NativePrelink/emit_prelink.py (sparse prelink index,
coalesced __PRELINK_TEXT/__PLK_TEXT_EXEC/__PLK_DATA_CONST/__PRELINK_DATA, XML prelink info with
_PrelinkLinkKASLROffsets), with what an arm64e MH_FILESET source needs on top:
  * code: every kext's __TEXT_EXEC translated in kernel mode by a64x (tools/a64kext: PAC as the identity, LSE and FP
    stubs, MSR/MRS DIT/SSBS/PAN/UAO, SB), each kext's stubs in its own block of a stub area after the kexts' text;
  * pointers: every DYLD_CHAINED_PTR_64_KERNEL_CACHE fixup (authenticated or not) written as a plain pointer;
    kext -> kext: target + slide; kext -> kernel: our kernel's address of the same name (prelink26/names.py);
  * branches: the kernel collection builder removed the kexts' stubs (BL straight into Apple's kernel), so every
    B/BL into the kernel is re-aimed at our kernel's function of the same name;
  * every kernel target has a name (names.py, pinned_names.py, shims.py): the prelink refuses any it is missing,
    with the kexts that import it.
usage: emit26.py OUR_KERNEL KERNELCACHE OUTPUT   (writes OUTPUT and OUTPUT.json)
"""
import base64, bisect, collections, copy, hashlib, json, os, struct, subprocess, sys, tempfile
import xml.etree.ElementTree as ET
HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.join(HERE, "..")
sys.path.insert(0, HERE)
from kcinfo import KC, ROOTS, decode8
import paths
from tools.common.macho import macho, exports, backed_offset

CUTS = {("com.apple.iokit.IOUSBHostFamily", "com.apple.driver.AppleSMC"): "_switch_prelink_cut_null",
        ("com.apple.iokit.IOUSBHostFamily", "com.apple.driver.usb.AppleUSBHostPlatformProperties"): None,
        ("com.apple.iokit.IOAccessoryManager", "com.apple.driver.AppleOnboardSerial"): "_switch_prelink_cut_null"}
DEFERRED_PERSONALITIES = {"com.apple.iokit.IOUSBHostFamily": ("AppleUSBHostResources", "AppleUSBHostResourcesTypeC"),
                          "com.apple.iokit.IOUSBMassStorageDriver": ("IOUSBMassStorageResource",),
                          "com.apple.driver.usb.AppleUSBHostPacketFilter": ("AppleUSBHostPacketFilter",)}
COALESCED = {"__PRELINK_TEXT": "__TEXT", "__PLK_DATA_CONST": "__DATA_CONST", "__PLK_TEXT_EXEC": "__TEXT_EXEC",
             "__PRELINK_DATA": "__DATA"}
M64 = (1 << 64) - 1
PAGE = 0x4000


def to_xml(value):
    if isinstance(value, dict):
        node = ET.Element("dict")
        for key, child in value.items():
            ET.SubElement(node, "key").text = key
            node.append(to_xml(child))
    elif isinstance(value, list):
        node = ET.Element("array"); node.extend(to_xml(c) for c in value)
    elif isinstance(value, bool):
        node = ET.Element("true" if value else "false")
    elif isinstance(value, int):
        node = ET.Element("integer", {"size": "64"}); node.text = hex(value & M64)
    elif isinstance(value, bytes):
        node = ET.Element("data"); node.text = base64.b64encode(value).decode()
    elif isinstance(value, str):
        node = ET.Element("string"); node.text = value
    else:
        raise ValueError(type(value))
    return node


def sx(v, b): return v - (1 << b) if v & (1 << (b - 1)) else v


LC_SEGMENT_SPLIT_INFO = 0x1E


def first_section(kc, fo, vm):
    """The lowest section address of the kext whose header is at file offset fo (unslid address vm)."""
    addresses = []
    for cmd, p, size in kc.entry_cmds(fo):
        if cmd == 0x19:
            for k in range(struct.unpack_from("<I", kc.d, p + 64)[0]):
                addresses.append(struct.unpack_from("<Q", kc.d, p + 72 + 80 * k + 32)[0])
    return min((a for a in addresses if a >= vm), default=vm)


def mark_split(blob, at, vm, kc, fo, name):
    """Give the kext header at blob[at] an empty LC_SEGMENT_SPLIT_INFO, as kcgen leaves on every kext of a prelinked
    kernelcache (21H461's: dataoff and datasize 0).

    XNU's prelinked-kernelcache paths take it to mean the kext's segments are scattered through the cache, which ours
    are; a fileset's kexts do not carry it. Without it kalloc_type_view_parse panics ("No support for non-split seg
    KCs") before the console, OSKext::setVMAttributes re-protects the kext as one range (its start then faults) and
    slidePrelinkedExecutable frees what it takes for the kext's tail.
    """
    ncmds, sizeofcmds = struct.unpack_from("<II", blob, at + 16)
    end = at + 32 + sizeofcmds
    if vm + 32 + sizeofcmds + 16 > first_section(kc, fo, vm) or any(blob[end:end + 16]):
        raise SystemExit("%s: no header padding for LC_SEGMENT_SPLIT_INFO" % name)
    struct.pack_into("<4I", blob, end, LC_SEGMENT_SPLIT_INFO, 16, 0, 0)
    struct.pack_into("<II", blob, at + 16, ncmds + 1, sizeofcmds + 16)


def write_fips_macs(kexts, kc, blobs, blob_at):
    """Give every kext with corecrypto's FIPS integrity check the MAC of its text as it now runs; the rule and the
    MAC are the pretranslation's too (translate/arm64e/macho/fips.h). Returns {kext: new MAC (hex)}."""
    macs = {}
    with tempfile.TemporaryDirectory() as t:
        headers = []
        for b in kexts:
            n, at = blob_at(kc.entry[b][0])
            sizeofcmds = struct.unpack_from("<I", blobs[n], at + 20)[0]
            path = os.path.join(t, "%d.h" % len(headers))
            open(path, "wb").write(blobs[n][at:at + 32 + sizeofcmds])
            headers.append((b, path))
        found = subprocess.run([paths.ARM64E, "fips", *[p for _, p in headers]], check=True, capture_output=True,
                               text=True).stdout.split()
        lines = iter(found)
        for b, _ in headers:
            first = next(lines)
            if first == "-":
                continue
            text_va, size, mac_va = int(first, 16), int(next(lines), 16), int(next(lines), 16)
            n, at = blob_at(text_va)
            m, mac_at = blob_at(mac_va)
            original, translated = os.path.join(t, "original"), os.path.join(t, "translated")
            open(original, "wb").write(kc.d[kc.va2off(text_va):kc.va2off(text_va) + size])
            open(translated, "wb").write(blobs[n][at:at + size])
            was = bytes(blobs[m][mac_at:mac_at + 32]).hex()
            mac = subprocess.run([paths.ARM64E, "fips-mac", original, translated, was], check=True,
                                 capture_output=True, text=True).stdout.strip()
            blobs[m][mac_at:mac_at + 32] = bytes.fromhex(mac)
            if mac != was:
                macs[b] = mac
    return macs


def main():
    our_path, kc_path, out_path = sys.argv[1:4]
    native_data = open(our_path, "rb").read()
    native = macho(native_data)
    nsym = {}
    for a, ns in exports(native_data, native).items():
        for n in ns: nsym.setdefault(n, a)
    for l in open(paths.work("ours.nm")):          # local symbols too (vtable slots name some)
        a, t, n = l.split(); nsym.setdefault(n, int(a, 16))
    # every defined symbol of ours (exports() keeps external ones only; kexts bind to KPI = external)
    marker = nsym.get("_kalloc_sparse_prelink_index_version")
    if marker is None or struct.unpack_from("<I", native_data, backed_offset(native["segments"], marker, 4, len(native_data)))[0] != 1:
        raise SystemExit("our kernel lacks the sparse prelink index ABI v1")
    kc = KC(kc_path)
    d = kc.d
    missing = [r for r in ROOTS if r not in kc.plist]
    if missing:
        raise SystemExit("roots this kernelcache does not ship: %s" % ", ".join(missing))
    sel, ifaces, codeless = kc.closure(ROOTS, set(CUTS))
    names = {int(a, 16): n for a, n in json.load(open(paths.work("kernel-names.json")))["names"].items()}
    # kexts link against the KPI only: a kext target named outside the 26.5.2 symbol sets (or iOS 17's imports) is
    # a naming error, so it stays unresolved
    import re
    kpi = set(l.strip() for l in open(paths.work("symbolsets26.txt")))
    # symbol-set entries like __ZN16IOPlatformExpert are prefixes (every member of the class)
    prefixes = tuple(e for e in kpi if re.fullmatch(r"__ZN(\d+[A-Za-z_]\w*?)+", e) and not e.endswith("E"))
    source = json.load(open(paths.work("kernel-names.json")))["source"]
    def exported(a, n):
        return (n in kpi or n.startswith(prefixes) or n.startswith("__ZTV") or n.endswith("gMetaClassE")
                or source.get(hex(a)) in ("vtable", "got17", "got17+order", "symbolicator-class", "shim"))
    names = {a: n for a, n in names.items() if exported(a, n)}
    # extents
    ext = []
    segs_of = {}
    for bid, vm, fo in kc.entries:
        segs_of[bid] = kc.entry_segs(fo)
        for s in segs_of[bid]:
            if s["size"] and s["name"] != "__LINKEDIT": ext.append((s["va"], s["va"] + s["size"], bid, s["name"]))
    ext.sort(); starts = [e[0] for e in ext]
    def owner(a):
        i = bisect.bisect_right(starts, a) - 1
        return ext[i] if i >= 0 and a < ext[i][1] else None
    kernel_text = [(s["va"], s["va"] + s["size"]) for s in segs_of["com.apple.kernel"] if s["initprot"] & 4]
    our_text = [(s["va"], s["va"] + s["size"]) for s in native["segments"] if s["initprot"] & 4 and s["size"]]
    def in_our_text(a): return any(lo <= a < hi for lo, hi in our_text)
    # coalesced source ranges (trimmed to the selected kexts, page aligned)
    rng = {}
    for out_name, seg_name in COALESCED.items():
        lo = min(s["va"] for b in sel for s in segs_of[b] if s["name"] == seg_name and s["size"])
        hi = max(s["va"] + s["size"] for b in sel for s in segs_of[b] if s["name"] == seg_name and s["size"])
        rng[out_name] = (lo & ~(PAGE - 1), (hi + PAGE - 1) & ~(PAGE - 1))
    text_lo, text_hi = rng["__PLK_TEXT_EXEC"]
    stub_va = text_hi                        # the stub area follows the kexts' text (same segment)
    stub_reserve = 0x100000
    nxt_after_text = min(lo for n, (lo, hi) in rng.items() if lo > text_lo)
    if stub_va + stub_reserve > nxt_after_text:
        raise SystemExit("no room for the stub area after the kexts' text")
    lowest = min(lo for lo, hi in rng.values())
    highest = max(hi for lo, hi in rng.values())
    kbase = min(s["va"] for s in native["segments"] if s["size"])
    limit = (kbase - 0x100000 - highest) & ~(PAGE - 1)
    limit = limit - (1 << 64) if limit >= 1 << 63 else limit
    slide = limit if paths.SLIDE is None else paths.SLIDE
    if slide > limit or slide % PAGE:
        raise SystemExit("slide %#x: the kexts must sit page-aligned at or below %#x" % (slide & M64, limit & M64))
    for lo, hi in our_text:
        for a, b in ((text_lo + slide, lo), (stub_va + stub_reserve + slide, hi)):
            if abs(a - b) >= 1 << 27:
                raise SystemExit("kext text out of BL range of our kernel")
    # 1. copy the selected kexts' segments
    blobs = {n: bytearray(hi - lo) for n, (lo, hi) in rng.items()}
    def blob_at(va):
        for n, (lo, hi) in rng.items():
            if lo <= va < hi: return n, va - lo
        return None, None
    copied = collections.Counter()
    for b in sorted(sel):
        for s in segs_of[b]:
            if s["name"] == "__LINKEDIT" or not s["filesize"]: continue
            n, o = blob_at(s["va"])
            if n is None or COALESCED[n] != s["name"]:
                raise SystemExit("kext segment outside its coalesced range: %s %s" % (b, s["name"]))
            fo = kc.va2off(s["va"])
            blobs[n][o:o + s["filesize"]] = d[fo:fo + s["filesize"]]
            copied[n] += s["filesize"]
    # 2. code: translate every kext's instruction sections
    with tempfile.TemporaryDirectory() as t:
        jobs = os.path.join(t, "jobs")
        with open(jobs, "w") as f:
            for b in sorted(sel):
                f.write("K %s\n" % b)
                for s in segs_of[b]:
                    for sec in s["sections"]:
                        if sec["flags"] & 0x80000400 and sec["size"]:
                            f.write("R %x %x\n" % (sec["va"], sec["size"]))
                vm, fo = kc.entry[b]
                text_vm = next(s["va"] for s in segs_of[b] if s["name"] == "__TEXT")
                for cmd, p, size in kc.entry_cmds(fo):
                    if cmd == 0x26:
                        off, sz = struct.unpack_from("<II", d, p + 8)
                        a = text_vm; q = off
                        while q < off + sz:
                            v = 0; sh = 0
                            while True:
                                byte = d[q]; q += 1; v |= (byte & 0x7F) << sh; sh += 7
                                if not byte & 0x80: break
                            if not v: break
                            a += v; f.write("F %x\n" % a)
                f.write("E\n")
        open(os.path.join(t, "text"), "wb").write(blobs["__PLK_TEXT_EXEC"])
        r = subprocess.run([paths.ARM64E, "kext", os.path.join(t, "text"), hex(text_lo), jobs, hex(stub_va),
                            os.path.join(t, "text.out"), os.path.join(t, "stubs")], capture_output=True, text=True)
        if r.returncode: raise SystemExit(r.stderr)
        blobs["__PLK_TEXT_EXEC"] = bytearray(open(os.path.join(t, "text.out"), "rb").read())
        stubs = bytearray(open(os.path.join(t, "stubs"), "rb").read())
    xlate = {}
    islands = []
    for l in r.stdout.splitlines():
        p = l.split()
        if p[0] == "I":
            islands.append((int(p[1], 16), int(p[1], 16) + 4 * int(p[2]))); continue
        xlate[p[1]] = {"words": int(p[2]), "changed": int(p[3]), "stub_words": int(p[5]) - int(p[4]),
                       "stub_va": stub_va + 4 * int(p[4]) + slide,
                       "classes": {k: v for k, v in (x.split("=") for x in p[6:])}}
    # 3. pointers
    pointer_sites = []
    unresolved_fn, unresolved_data = collections.Counter(), collections.Counter()
    unresolved_by_component = collections.defaultdict(lambda: {"functions": set(), "data": set()})
    cut_null = nsym.get("_switch_prelink_cut_null")
    kmod_address_sites = {(kc.plist[b]["_PrelinkKmodInfo"] & M64) + 156 for b in sel}
    kinds = collections.Counter()
    fixup_sites = set()
    for site, raw, fmt in kc.fixups_all():
        o = owner(site)
        if not o or o[2] not in sel: continue
        f = decode8(raw); tgt = kc.base + f["target"]
        t = owner(tgt)
        if t and t[2] in sel:
            new = tgt + slide; kinds["kext"] += 1
        elif t and t[2] == "com.apple.kernel":
            n = names.get(tgt)
            if n and n in nsym:
                new = nsym[n]; kinds["kernel"] += 1
            elif any(lo <= tgt < hi for lo, hi in kernel_text):
                new = 0; unresolved_fn[tgt] += 1; kinds["kernel unresolved function"] += 1
                unresolved_by_component[o[2]]["functions"].add(tgt)
            else:
                new = 0; unresolved_data[tgt] += 1; kinds["kernel unresolved data"] += 1
                unresolved_by_component[o[2]]["data"].add(tgt)
        elif t and (o[2], t[2]) in CUTS:
            new = cut_null; kinds["cut"] += 1
        else:
            raise SystemExit("pointer from %s to unselected %s" % (o[2], t and t[2]))
        n_, at = blob_at(site)
        struct.pack_into("<Q", blobs[n_], at, new & M64)
        fixup_sites.add(site)
        if site in kmod_address_sites:
            kinds["kmod_info.address (slid by OSKext, not listed)"] += 1
        else:
            pointer_sites.append(site + slide)
    # 4. branches into the kernel
    br = collections.Counter()
    islands.sort(); isl_starts = [x[0] for x in islands]
    text = blobs["__PLK_TEXT_EXEC"]
    # The stub area follows the kexts' text in the kernelcache's frame, where Apple's kernel text begins: a branch a64x
    # aimed at one of its stubs is the kext's own, not a kernel call to rebind.
    stubs_end = stub_va + len(stubs)
    for b in sorted(sel):
        for s in segs_of[b]:
            for sec in s["sections"]:
                if not (sec["flags"] & 0x80000400 and sec["size"]): continue
                o0 = sec["va"] - text_lo
                for i in range(sec["size"] // 4):
                    w = struct.unpack_from("<I", text, o0 + 4 * i)[0]
                    if w & 0x7C000000 != 0x14000000: continue
                    pc = sec["va"] + 4 * i
                    j = bisect.bisect_right(isl_starts, pc) - 1
                    if j >= 0 and pc < islands[j][1]: br["in data island, left"] += 1; continue
                    tgt = pc + (sx(w & 0x3FFFFFF, 26) << 2)
                    if stub_va <= tgt < stubs_end: br["a64x stub"] += 1; continue
                    if not any(lo <= tgt < hi for lo, hi in kernel_text): continue
                    n = names.get(tgt)
                    if n and n in nsym and in_our_text(nsym[n]):
                        dest = nsym[n]; br["kernel"] += 1
                    else:
                        unresolved_fn[tgt] += 1; br["kernel unresolved"] += 1
                        unresolved_by_component[b]["functions"].add(tgt)
                        continue
                    off = dest - (pc + slide)
                    if not -(1 << 27) <= off < (1 << 27): raise SystemExit("branch out of range at %x" % pc)
                    struct.pack_into("<I", text, o0 + 4 * i, (w & 0xFC000000) | ((off >> 2) & 0x3FFFFFF))
    stub_area = stubs
    if len(stub_area) > stub_reserve: raise SystemExit("stub area overflow")
    text.extend(bytes(stub_va - text_lo - len(text)))
    text.extend(stub_area)
    text.extend(bytes(-len(text) % PAGE))
    # 4b. FIPS integrity MACs over the final text, while the headers still hold the kernelcache's addresses
    fips_macs = write_fips_macs(sorted(sel), kc, blobs, blob_at)
    # 5. kext headers: slid segment/section addresses, arm64 subtype ALL, no LINKEDIT mapping
    for b in sorted(sel):
        vm, fo = kc.entry[b]
        n_, at = blob_at(vm)
        blob = blobs[n_]
        struct.pack_into("<I", blob, at + 8, 0)          # cpusubtype: CPU_SUBTYPE_ARM64_ALL, no ptrauth ABI
        mark_split(blob, at, vm, kc, fo, b)
        for cmd, p, size in kc.entry_cmds(fo):
            if cmd != 0x19: continue
            q = at + (p - fo)
            name = bytes(blob[q + 8:q + 24]).rstrip(b"\0")
            va, vs = struct.unpack_from("<QQ", blob, q + 24)
            if name == b"__LINKEDIT":
                struct.pack_into("<QQ", blob, q + 24, (va + slide) & M64, 0)
                struct.pack_into("<Q", blob, q + 48, 0)
                continue
            struct.pack_into("<Q", blob, q + 24, (va + slide) & M64)
            nsects = struct.unpack_from("<I", blob, q + 64)[0]
            for k in range(nsects):
                sq = q + 72 + 80 * k
                a = struct.unpack_from("<Q", blob, sq + 32)[0]
                if a: struct.pack_into("<Q", blob, sq + 32, (a + slide) & M64)
    # kmod_info.address (+156) is slid by OSKext itself, as in the main tree's prelink
    kmods = {}
    for b in sorted(sel):
        e = kc.plist[b]
        km = e["_PrelinkKmodInfo"] & M64
        n_, at = blob_at(km)
        addr = struct.unpack_from("<Q", blobs[n_], at + 156)[0]
        kmods[b] = hex(km + slide)
        if km + 156 in fixup_sites or not addr:
            continue                              # written in step 3 (target + slide), kept out of the KASLR list
        if (addr >> 48) != 0xffff:
            raise SystemExit("kmod_info.address of %s is not a plain address: %x" % (b, addr))
        struct.pack_into("<Q", blobs[n_], at + 156, (addr + slide) & M64)
    # 6. append the segments to our kernel
    output = bytearray(native_data)
    new_segments = []
    def append_segment(name, va, blob, protection):
        size = len(blob)
        placeholder = next((s for s in native["segments"] if s["name"] == name), None)
        if placeholder is None or placeholder["size"] or len(placeholder["sections"]) != 1:
            raise SystemExit("expected one empty %s segment/section in our kernel" % name)
        for s in [*native["segments"], *new_segments]:
            if s["size"] and va < s["va"] + s["size"] and s["va"] < va + size:
                raise SystemExit("%s collides with %s" % (name, s["name"]))
        fileoff = (len(output) + PAGE - 1) & ~(PAGE - 1)
        output.extend(bytes(fileoff - len(output))); output.extend(blob)
        command = placeholder["command_offset"]
        struct.pack_into("<4Q2I", output, command + 24, va & M64, size, fileoff, size, protection, protection)
        struct.pack_into("<QQI", output, placeholder["sections"][0]["command_offset"] + 32, va & M64, size, fileoff)
        new_segments.append({"name": name, "va": va & M64, "size": size, "filesize": size, "fileoff": fileoff})
    prot = {"__PRELINK_TEXT": 1, "__PLK_TEXT_EXEC": 5, "__PLK_DATA_CONST": 1, "__PRELINK_DATA": 3}
    for n in ("__PRELINK_TEXT", "__PLK_DATA_CONST", "__PLK_TEXT_EXEC", "__PRELINK_DATA"):
        b = blobs[n]
        b.extend(bytes(-len(b) % PAGE))
        append_segment(n, rng[n][0] + slide, b, prot[n])
    # 7. sparse header index at the end of __PRELINK_TEXT, prelink info
    prelink_text = next(s for s in new_segments if s["name"] == "__PRELINK_TEXT")
    headers = sorted((kc.plist[b]["_PrelinkExecutableLoadAddr"] & M64) + slide for b in sel)
    index_size = 8 * len(headers)
    occupied = max(s["va"] + s["size"] for b in sel for s in segs_of[b] if s["name"] == "__TEXT") + slide
    index_va = ((prelink_text["va"] + prelink_text["size"] - index_size) & ~7)
    if index_va < occupied & M64:
        raise SystemExit("no room for the sparse index in __PRELINK_TEXT")
    index_at = prelink_text["fileoff"] + index_va - prelink_text["va"]
    struct.pack_into("<%dQ" % len(headers), output, index_at, *[h & M64 for h in headers])
    entries = []
    for b in sorted(ifaces):
        e = copy.deepcopy(kc.plist[b])
        for k in ("_PrelinkExecutableLoadAddr", "_PrelinkExecutableSourceAddr"):
            e.pop(k, None)
        entries.append(e)
    for b in [*sorted(sel), *sorted(codeless)]:
        e = copy.deepcopy(kc.plist[b])
        for k in ("_PrelinkExecutableLoadAddr", "_PrelinkExecutableSourceAddr", "_PrelinkKmodInfo"):
            if e.get(k) and b in sel: e[k] = ((e[k] & M64) + slide) & M64
        for (c, dep) in CUTS:
            if c == b: e.get("OSBundleLibraries", {}).pop(dep, None)
        for p in DEFERRED_PERSONALITIES.get(b, ()):
            e.get("IOKitPersonalities", {}).pop(p, None)
        entries.append(e)
    base_va = prelink_text["va"]
    offsets = sorted(((s & M64) - base_va) for s in pointer_sites)
    if offsets and (offsets[0] < 0 or offsets[-1] > 0xFFFFFFFF): raise SystemExit("KASLR offset outside 32 bits")
    metadata = {"_PrelinkInfoDictionary": entries,
                "_PrelinkLinkKASLROffsets": struct.pack("<%dI" % (len(offsets) + 1), len(offsets), *offsets),
                "_PrelinkKCID": hashlib.sha256(b"SWITCH-26.5.2-A64X-KCID\0" + native_data + hashlib.sha256(d).digest()).digest()[:16]}
    xml = ET.tostring(to_xml(metadata), encoding="utf-8").replace(b" />", b"/>") + b"\0"
    xml += bytes(-len(xml) % PAGE)
    info_va = (max(s["va"] + s["size"] for s in native["segments"] if s["size"]) + PAGE - 1) & ~(PAGE - 1)
    append_segment("__PRELINK_INFO", info_va, xml, 3)
    commands = []
    for command, at, length in native["commands"]:
        blob = bytearray(output[at:at + length])
        if command == 0x19 and blob[8:24].rstrip(b"\0") == b"__PRELINK_TEXT":
            struct.pack_into("<I", blob, 4, length + 80)
            struct.pack_into("<I", blob, 64, 2)
            struct.pack_into("<Q", blob, 72 + 40, index_va - prelink_text["va"])
            blob.extend(struct.pack("<16s16sQQ8I", b"__macho_list", b"__PRELINK_TEXT", index_va, index_size, index_at,
                                    3, 0, 0, 0, 0, 0, 0))
        commands.append(blob)
    rebuilt = b"".join(commands)
    first_payload = min(sec["fileoff"] for s in native["segments"] for sec in s["sections"] if sec["size"] and sec["fileoff"])
    if 32 + len(rebuilt) > first_payload: raise SystemExit("no header padding for the sparse index section")
    struct.pack_into("<I", output, 20, len(rebuilt))
    output[32:32 + len(rebuilt)] = rebuilt
    macho(bytes(output))
    open(out_path, "wb").write(output)
    report = {"kernelcache_sha256": hashlib.sha256(d).hexdigest(), "native_sha256": hashlib.sha256(native_data).hexdigest(),
              "output_sha256": hashlib.sha256(output).hexdigest(), "output_bytes": len(output), "slide": slide,
              "components": sorted(sel), "codeless_components": sorted(codeless), "kernel_interfaces": sorted(ifaces),
              "cuts": [list(k) for k in CUTS],
              "segments": [{k: (hex(v) if isinstance(v, int) else v) for k, v in s.items()} for s in new_segments],
              "pointer_kinds": dict(kinds), "pointer_sites": len(pointer_sites), "branch_kinds": dict(br),
              "fips_macs": fips_macs,
              "unresolved_kernel_functions": {hex(a): c for a, c in sorted(unresolved_fn.items())},
              "unresolved_kernel_data": {hex(a): c for a, c in sorted(unresolved_data.items())},
              "unresolved_by_component": {b: {k: [hex(a) for a in sorted(v)] for k, v in u.items()}
                                          for b, u in sorted(unresolved_by_component.items())},
              "translation": xlate, "stub_area": {"va": hex(stub_va + slide), "a64x_words": len(stubs) // 4},
              "kmod_info": kmods, "sparse_index": {"va": hex(index_va), "headers": len(headers)},
              "early_security_initializers": {}, "runtime_qualified": False}
    json.dump(report, open(out_path + ".json", "w"), indent=1)
    print("kexts %d, slide %#x, output %d bytes" % (len(sel), slide & M64, len(output)))
    print("pointers", dict(kinds)); print("branches", dict(br))
    print("unresolved kernel functions %d (sites %d), data %d (sites %d)" % (len(unresolved_fn), sum(unresolved_fn.values()),
          len(unresolved_data), sum(unresolved_data.values())))
    tot = collections.Counter()
    for x in xlate.values():
        tot["words"] += x["words"]; tot["changed"] += x["changed"]; tot["stub_words"] += x["stub_words"]
    print("translation", dict(tot))
    if unresolved_fn or unresolved_data:
        raise SystemExit("emit26.py: kernel targets the kexts use that nothing names (names.py, pinned_names.py, "
                         "shims.py):\n" + "\n".join("  %s: functions %s, data %s" % (b, sorted(map(hex, u["functions"])),
                                                                                       sorted(map(hex, u["data"])))
                                                   for b, u in sorted(unresolved_by_component.items())))


if __name__ == "__main__":
    main()
