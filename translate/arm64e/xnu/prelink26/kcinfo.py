#!/usr/bin/env python3
"""26.5.2 fileset: prelink info, dependency closure of our roots, chained fixups (DYLD_CHAINED_PTR_64_KERNEL_CACHE)."""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from fileset import Fileset
import base64, xml.etree.ElementTree as ET

# main's 17 roots (passes/native-root-smp-v1/prelink-command.json), same bundle ids in 26.5.2
ROOTS = ["com.apple.driver.AppleMobileFileIntegrity", "com.apple.filesystems.apfs", "com.apple.security.sandbox",
         "com.apple.AppleFSCompression.AppleFSCompressionTypeZlib", "com.apple.iokit.IOMobileGraphicsFamily",
         "com.apple.iokit.IOSurface", "com.apple.iokit.IOHIDFamily", "com.apple.driver.DiskImages",
         "com.apple.driver.DiskImages.FileBackingStore", "com.apple.driver.DiskImages.KernelBacked",
         "com.apple.driver.DiskImages.ReadWriteDiskImage", "com.apple.driver.AppleGameControllerPersonality",
         "com.apple.driver.AppleUSBDeviceMux", "com.apple.driver.usb.AppleUSBCommon",
         "com.apple.driver.usb.AppleUSBHostCompositeDevice", "com.apple.driver.usb.AppleUSBHub",
         "com.apple.driver.usb.AppleUSBXHCI", "com.apple.driver.usb.IOUSBHostHIDDevice",
         "com.apple.iokit.IOAccessoryManager", "com.apple.iokit.IOGPUFamily", "com.apple.iokit.IOPCIFamily",
         "com.apple.iokit.IOPortFamily", "com.apple.iokit.IOSCSIArchitectureModelFamily",
         "com.apple.iokit.IOSCSIBlockCommandsDevice", "com.apple.iokit.IOUSBDeviceFamily",
         "com.apple.iokit.IOUSBHostFamily", "com.apple.iokit.IOUSBMassStorageDriver",
         "com.apple.driver.AppleUSBCardReader", "com.apple.driver.AppleUSBMassStorageInterfaceNub",
         "com.apple.driver.USBStorageDeviceSpecifics", "com.apple.driver.SCSIDeviceSpecifics",
         "com.apple.filesystems.lifs", "com.apple.driver.usb.AppleUSBHostPacketFilter",
         "com.apple.driver.usb.AppleUSBHostDeviceSupport", "com.apple.driver.usb.cdc", "com.apple.driver.usb.cdc.acm",
         "com.apple.driver.usb.serial", "com.apple.driver.usb.networking", "com.apple.iokit.IOSerialFamily",
         "com.apple.driver.usb.IOUSBHostHIDDeviceSafeBoot", "com.apple.iokit.IOHIDEventDriver",
         "com.apple.iokit.IOHIDEventDriverSafeBoot", "com.apple.driver.AppleHIDKeyboard",
         "com.apple.driver.AppleHIDKeyboardEmbedded", "com.apple.driver.AppleTopCaseDriverV2",
         "com.apple.driver.AppleTopCaseHIDEventDriver", "com.apple.driver.AppleUSBTopCaseDriver",
         "com.apple.driver.AppleMultitouchDriver", "com.apple.driver.AppleMultitouchSPI",
         "com.apple.driver.AppleActuatorDriver", "com.apple.driver.ApplePencilDriver",
         "com.apple.driver.AppleInputDeviceSupport", "com.apple.driver.IOSlaveProcessor",
         "com.apple.driver.usb.AppleUSBHostiOSDevice", "com.apple.driver.AppleUSBHostMergeProperties",
         "com.apple.iokit.IOGameControllerFamily", "com.apple.driver.AppleUSBAudio",
         "com.apple.iokit.IONetworkingFamily", "com.apple.driver.usb.cdc.ecm", "com.apple.driver.usb.cdc.ncm",
         "com.apple.driver.usb.ethernet.asix", "com.apple.driver.AppleUSBEthernetHost"]
# _PrelinkExecutableLoadAddr of a codeless kext in a fileset kernelcache (XNU kOSKextCodelessKextLoadAddr).
CODELESS_LOAD_ADDR = 0x7FFFFFFFFFFFFFFF


def unserialize(blob):
    refs = {}
    def rd(e):
        if "IDREF" in e.attrib:
            return refs[e.attrib["IDREF"]]
        t = e.tag
        if t == "dict":
            c = list(e); v = {}
            for i in range(0, len(c), 2):
                v[rd(c[i])] = rd(c[i + 1])
        elif t == "array": v = [rd(x) for x in e]
        elif t in ("key", "string"): v = e.text or ""
        elif t == "integer": v = int(e.text or "0", 0)
        elif t == "data": v = base64.b64decode("".join((e.text or "").split()))
        elif t in ("true", "false"): v = t == "true"
        else: raise ValueError(t)
        if "ID" in e.attrib: refs[e.attrib["ID"]] = v
        return v
    root = ET.fromstring(blob.split(b"\0", 1)[0])
    if root.tag == "plist": root = root[0]
    return rd(root)


class KC(Fileset):
    def __init__(self, path):
        super().__init__(path)
        info = next(s for s in self.segs if s["name"] == "__PRELINK_INFO")
        sec = info["sections"][0]
        self.info = unserialize(self.d[sec["off"]:sec["off"] + sec["size"]])
        self.plist = {e["CFBundleIdentifier"]: e for e in self.info["_PrelinkInfoDictionary"]}
        self.entry = {n: (vm, fo) for n, vm, fo in self.entries}
        self.base = self.segs[0]["va"]

    def closure(self, roots, cuts=()):
        """roots' dependency closure: the kexts with code, the kernel interfaces and the codeless kexts (personalities
        only, in the prelink info and in no fileset entry)."""
        sel, ifaces, pend = set(), set(), list(roots)
        while pend:
            i = pend.pop()
            if i in sel or i in ifaces: continue
            e = self.plist[i]
            if e.get("OSKernelResource"):
                ifaces.add(i); continue
            sel.add(i)
            pend += [d for d in (e.get("OSBundleLibraries") or {}) if (i, d) not in cuts]
        codeless = {i for i in sel if i not in self.entry}
        for i in codeless:
            e = self.plist[i]
            if (e.get("_PrelinkKmodInfo") or e.get("_PrelinkExecutableLoadAddr", CODELESS_LOAD_ADDR) != CODELESS_LOAD_ADDR
                    or not e.get("IOKitPersonalities")):
                raise ValueError("a kext in no fileset entry must be codeless with personalities: " + i)
        return sel - codeless, ifaces, codeless

    def fixups_all(self):
        """yield (site_va, raw64) for every chained pointer in the fileset"""
        d = self.d
        off, size = self.fixups
        ver, starts_off, imports_off, syms_off, nimp, ifmt, sfmt = struct.unpack_from("<7I", d, off)
        so = off + starts_off
        nseg = struct.unpack_from("<I", d, so)[0]
        for i in range(nseg):
            o = struct.unpack_from("<I", d, so + 4 + 4 * i)[0]
            if not o: continue
            p = so + o
            ssize, page_size, fmt, seg_off, max_valid, page_count = struct.unpack_from("<IHHQIH", d, p)
            seg = self.segs[i]
            for pg in range(page_count):
                st = struct.unpack_from("<H", d, p + 22 + 2 * pg)[0]
                if st == 0xFFFF: continue
                if st & 0x8000:  # DYLD_CHAINED_PTR_START_MULTI: index into the overflow list, last has 0x8000
                    k, firsts = st & 0x7FFF, []
                    while True:
                        v = struct.unpack_from("<H", d, p + 22 + 2 * k)[0]
                        firsts.append(v & 0x7FFF)
                        if v & 0x8000: break
                        k += 1
                else:
                    firsts = [st]
                for first in firsts:
                    va = self.base + seg_off + pg * page_size + first
                    at = self.va2off(va)
                    assert at is not None, hex(va)
                    while True:
                        raw = struct.unpack_from("<Q", d, at)[0]
                        yield va, raw, fmt
                        nxt = (raw >> 51) & 0xFFF
                        if not nxt: break
                        at += nxt * 4; va += nxt * 4

def decode8(raw):
    """DYLD_CHAINED_PTR_64_KERNEL_CACHE: target 30, cacheLevel 2, diversity 16, addrDiv 1, key 2, next 12, isAuth 1"""
    return dict(target=raw & 0x3FFFFFFF, level=(raw >> 30) & 3, div=(raw >> 32) & 0xFFFF, addr_div=(raw >> 48) & 1,
                key=(raw >> 49) & 3, auth=raw >> 63)

if __name__ == "__main__":
    kc = KC(sys.argv[1])
    sel, ifaces, codeless = kc.closure(ROOTS)
    print("plist entries", len(kc.plist), "fileset entries", len(kc.entries))
    print("closure", len(sel), "interfaces", sorted(ifaces))
    print(sorted(sel))
    missing = [r for r in ROOTS if r not in kc.plist]
    print("roots missing in 26.5.2:", missing)
