#!/usr/bin/env python3
"""The board ADT (//boot:adt) with what iPadOS userland expects of a J120.

Also the radios' nodes, the overlay's union directories, the loader's zeroed slots and, with --smp, all four A57s.

    adt_edit.py --adt BASE --overlay-plan PLAN --bt-patch HCD --portrait-header H [--smp]
                [--a64x-cache-pretranslated LIST] [--switch-pretranslated-trust MODULE] --out ADT
"""

import argparse
import json
import struct
from collections.abc import Sequence
from pathlib import Path

from boot_config import portrait_native
from loader_abi import SEED_SOURCE_BYTES, SEED_SOURCE_PROPERTY, STAMPS_BYTES, STAMPS_PROPERTY
from native_xnu_layout import parse_adt

# USB 2.0 device mode on the USB-C port (drivers/SwitchUSB): the USB function set configd's USBDeviceConfiguration
# plug-in gives Apple's IOUSBDeviceFamily, by name from /System/Library/AppleUSBDevice/USBDeviceConfiguration.plist.
# The family goes on the bus only once every function of every configuration has a driver, so only sets whose
# drivers exist here work: 'unknownHardware' (AppleUSBMux + PTP: usbmuxd/Finder/Xcode/atc and ptpd photos) or
# 'standardMuxOnly' (AppleUSBMux alone, no userland function driver). J120's own stdMuxPTPEthValIDA needs a dozen
# function drivers this port lacks. None leaves /arm-io/usb-device out, and the driver then does nothing.
USB_DEVICE_CONFIGURATION = 'unknownHardware'
# USB 2.0 host mode on the same port (drivers/SwitchUSB SwitchXHCI): a keyboard, mouse or flash drive through a USB-C
# OTG adapter or an unpowered hub. True writes /arm-io/usb-xhci, the driver's switch; the role then follows the BM92T36.
USB_HOST = True
# The panel's colour gamut as iOS learns it: /product artwork-display-gamut, MobileGestalt's ArtworkDisplayGamut, whose
# HasExtendedColorDisplay is "== P3". 'sRGB' (what sRGB iPads carry) turns off deep colour everywhere: QuartzCore's
# CADeviceUseCIF10 (so the display reports no extended colours, and CADisplayMode, FBSDisplayConfiguration and UIScreen
# say sRGB), UIKit's _supportsDeepColor, and WebCore's screenSupportsExtendedColor, so WebKit tiles are BGRA8 rather
# than two-plane b3a8 and Core Animation stops making w30r surfaces, formats the GM20B only reaches through copies. 'P3'
# is J120's own. Asset catalog images that this IPSW ships only in P3 (some onboarding and tips art) are not found under
# sRGB. The kernel drops MobileGestalt's cache when this changes (kern_exec.c native_switch_gamut_cache).
DISPLAY_GAMUT = 'sRGB'


def adt_parse(data: bytes, pos: int = 0) -> tuple[list, int]:
    count, children = struct.unpack_from('<II', data, pos)
    pos += 8
    props, nodes = [], []
    for _ in range(count):
        name = data[pos : pos + 32].split(b'\0', 1)[0].decode()
        word = struct.unpack_from('<I', data, pos + 32)[0]
        size = word & 0x7FFFFFFF
        props.append([name, word, data[pos + 36 : pos + 36 + size]])
        pos += 36 + size + (-size % 4)
    for _ in range(children):
        node, pos = adt_parse(data, pos)
        nodes.append(node)
    return [props, nodes], pos


def adt_serialize(node: list) -> bytes:
    props, children = node
    out = struct.pack('<II', len(props), len(children))
    for name, word, value in props:
        out += name.encode().ljust(32, b'\0') + struct.pack('<I', word) + value + bytes(-len(value) % 4)
    return out + b''.join(adt_serialize(c) for c in children)


def put(node: list, name: str, value: bytes) -> None:
    for prop in node[0]:
        if prop[0] == name:
            prop[1:] = [(prop[1] & 0x80000000) | len(value), value]
            return
    node[0].append([name, len(value), value])


def child(node: list, name: str) -> list:
    for c in node[1]:
        if any(p[0] == 'name' and p[2].split(b'\0')[0] == name.encode() for p in c[0]):
            return c
    c = [[['name', len(name) + 1, name.encode() + b'\0']], []]
    node[1].append(c)
    return c


def u32(value: int) -> bytes:
    return struct.pack('<I', value)


def four_cores(tree: list) -> None:
    """All four A57s (MPIDR 0-3, one cluster, shared L2) started by PSCI CPU_ON over SMC."""
    cpus = child(tree, 'cpus')
    for core in range(4):
        cpu = child(cpus, f'cpu{core}')
        put(cpu, 'reg', u32(core))
        put(cpu, 'device_type', b'cpu\0')
        put(cpu, 'compatible', b'arm,cortex-a57\0')
        put(cpu, 'timebase-frequency', u32(19200000))
        if core:
            put(cpu, 'state', b'stopped\0')
        for key, value in (('cluster-core-id', core), ('cluster-type', 0), ('die-cluster-id', 0), ('l2-cache-id', 0)):
            put(cpu, key, u32(value))
    psci = child(tree, 'psci')
    put(psci, 'compatible', b'arm,psci-0.2\0')
    put(psci, 'method', b'smc\0')


def _identity(tree: list) -> None:
    put(tree, 'model', b'iPad7,1\0')  # hw.model; idle code matches "compatible"
    put(tree, 'target-type', b'J120\0')
    # The audio server keys its routing database on the board name; the Switch entries stay
    # after it (switch_idle_board looks for nintendo,aula).
    compatible = next(p[2] for p in tree[0] if p[0] == 'compatible')
    if not compatible.startswith(b'J120AP\0'):
        put(tree, 'compatible', b'J120AP\0' + compatible)


def _chosen(tree: list, portrait: bool) -> list:
    chosen = child(tree, 'chosen')
    put(chosen, 'ephemeral-storage', u32(0))  # Data is a persistent APFS volume
    # Portrait-native kernel (SwitchPortrait.h): iOS is given the 720x1280 panel and runs landscape with the panel's
    # top on the user's right. MobileGestalt DisplayBootRotation reads this: SpringBoard starts in interface orientation
    # 4 (landscape left, -[SpringBoard _bootOrientation]) and ProgressUI turns the boot Apple logo the same way. The
    # kernel also restores SpringBoard's rotation lock to 4 when it is set (kern_exec.c). display-rotation stays 0
    # (published at runtime): a non-zero one would mean a panel mounted rotated, and iOS would composite sideways.
    if portrait:
        put(chosen, 'display-boot-rotation', u32(270))
    # Locally administered addresses; MobileGestalt waits on these when they are missing.
    for index, key in enumerate(('mac-address-wifi0', 'mac-address-bluetooth0', 'mac-address-ethernet0')):
        put(chosen, key, bytes([0x02, 0x1D, 0x5A, 0x00, 0x00, index + 1]))
    for key in ('certificate-production-status', 'effective-production-status-ap'):
        put(chosen, key, u32(0))
    for key in ('certificate-security-mode', 'effective-security-mode-sep'):
        put(chosen, key, u32(1))
    return chosen


def _product(tree: list) -> None:
    product = child(tree, 'product')
    for key, value in (
        ('product-name', b'iPad Pro (12.9-inch) (2nd generation)\0'),
        ('product-description', b'iPad Pro (12.9-inch) (2nd generation)\0'),
        ('model', b'iPad7,1\0'),
        ('artwork-device-idiom', b'pad\0'),
        ('artwork-device-subtype', u32(2732)),
        ('artwork-scale-factor', u32(2)),
        ('artwork-display-gamut', DISPLAY_GAMUT.encode() + b'\0'),
        ('compatible-device-fallback', b'iPad6,7\0'),
        ('thin-bezel', u32(1)),
        ('product-id', bytes.fromhex('e9fefef6e6f9d12a5ba1a2551866cbbd749f0ec3')),
        # MobileGestalt's GraphicsFeatureSetClass and GraphicsFeatureSetFallbacks, by which CoreUI picks asset catalog
        # renditions (without them every CUICatalog logs that kMGArtworkGraphicsFeatureSetClass has no answer).
        ('graphics-featureset-class', b'APPLE3\0'),
        ('graphics-featureset-fallbacks', b'APPLE3v1:APPLE2:APPLE1:GLES2,0\0'),
        ('device-perf-memory-class', u32(3)),
        ('app-macho-architecture', b'arm64\0'),
        ('framebuffer-identifier', b'0\0'),
        ('display-mirroring', u32(1)),
        ('ui-background-quality', u32(100)),
        ('ui-weather-quality', u32(100)),
    ):
        put(product, key, value)
    put(child(product, 'audio'), 'wireless-splitter', u32(1))  # as UniXNU's 21H461 profile


def _radios(tree: list, bt_patch: bytes | None) -> None:
    # Bluetooth (drivers/SwitchBluetooth). ArmDTPlatform publishes every /arm-io child, so this is the
    # IOService named "bluetooth" bluetoothd looks up (local-mac-address, transport-speed, transport-encoding: J120's 3,
    # H4BC, which also opens /dev/btwake, SwitchBluetooth's; bluetoothd takes its transport from kern_exec.c's
    # BT_HCI_TRANSPORT, the same H4BC, as plain H4 has no prepare-to-terminate step and crashed whenever Bluetooth was
    # turned off). The kernel downloads switch,patchram itself, since BlueTool has no BCM4356A3 firmware, and replaces
    # local-mac-address (and /chosen mac-address-bluetooth0) with the unit's own address from the fuse lot code when it
    # can read it.
    bluetooth = child(child(tree, 'arm-io'), 'bluetooth')
    for key, value in (
        ('device_type', b'bluetooth\0'),
        ('local-mac-address', bytes([0x02, 0x1D, 0x5A, 0x00, 0x00, 0x02])),
        ('transport-speed', u32(3000000)),
        ('transport-encoding', u32(3)),
    ):
        put(bluetooth, key, value)
    if bt_patch is not None:
        put(bluetooth, 'switch,patchram', bt_patch)
    # Wi-Fi (drivers/SwitchWLAN): /arm-io/wlan as J120 has it. The kernel replaces local-mac-address (and /chosen
    # mac-address-wifi0) with the unit's own address from the fuse lot code, the one the chip is given, before userland
    # starts.
    wlan = child(child(tree, 'arm-io'), 'wlan')
    for key, value in (('device_type', b'wlan\0'), ('local-mac-address', bytes([0x02, 0x1D, 0x5A, 0x00, 0x00, 0x01]))):
        put(wlan, key, value)


def _usb(tree: list) -> None:
    # USB device mode (drivers/SwitchUSB): /arm-io/usb-device with an iPad usb-device node's identity (J120: Apple
    # 0x05ac:0x12ab "iPad"), published by ArmDTPlatform; SwitchXUDC starts on it and IOUSBDeviceFamily reads these.
    # compatible is not usb-device,t8011, so nothing of Apple's matches the node itself.
    if USB_DEVICE_CONFIGURATION:
        usb = child(child(tree, 'arm-io'), 'usb-device')
        for key, value in (
            ('compatible', b'usb-device,tegra210\0'),
            ('device_type', b'usb-device\0'),
            ('vendor-id', u32(0x05AC)),
            ('product-id', u32(0x12AB)),
            ('product-version', u32(0x0701)),
            ('bcd-usb-version', u32(0x0200)),
            ('vendor-string', b'Apple Inc.\0'),
            ('manufacturer-string', b'Apple Inc.\0'),
            ('product-string', b'iPad\0'),
            ('configuration-string', USB_DEVICE_CONFIGURATION.encode() + b'\0'),
        ):
            put(usb, key, value)
    # USB host mode: /arm-io/usb-xhci (J120 has one), present = on. compatible is not usb-xhci,t8011, so nothing of
    # Apple's matches the node itself; SwitchXHCI makes its own provider with the register window and interrupt.
    if USB_HOST:
        xhci = child(child(tree, 'arm-io'), 'usb-xhci')
        for key, value in (('compatible', b'usb-xhci,tegra210\0'), ('device_type', b'usb-xhci\0')):
            put(xhci, key, value)


def _media_accel(tree: list) -> None:
    """The keys iOS's media frameworks read before they pick a hardware path."""
    # J120 has iommu-present (empty) on /arm-io. Its one reader in 21H461's dyld cache, VideoToolbox's
    # FigCreateIOSurfacePropertiesDictionary, otherwise asks every IOSurface it makes (decoder and pixel-transfer pools)
    # for the PurpleGfxMem carveout, which this tree does not have. The engines that touch those surfaces reach
    # scattered pages through their own MMUs: VIC, NVJPG and the display through the MC SMMU, the GPU through its GMMU.
    arm_io = child(tree, 'arm-io')
    put(arm_io, 'iommu-present', b'')
    # J120's A10X generation. MobileGestalt compares it in about 25 answers, among them HEVCDecoder8bitSupported (H8)
    # and HEVCDecoder10bitSupported (H9): absent, both answer false although NVDEC decodes HEVC. Its other reader in the
    # dyld cache is DeviceIdentity; the kernel only lists it as sandbox-readable.
    put(arm_io, 'soc-generation', b'H9\0')


def _capabilities(tree: list) -> None:
    """J120's own answers to what MobileGestalt reads before iOS offers a feature (libMobileGestalt, 21H461)."""
    # IOPlatformExpertDevice carries the root's properties; MobileGestalt's HardwarePlatform is its platform-name, which
    # iBoot sets to the SoC. Without it CoreVideo, AMS and the store's user agent read NULL.
    put(tree, 'platform-name', b't8011\0')
    put(tree, 'target-sub-type', b'J120AP\0')
    put(child(tree, 'chosen'), 'display-corner-radius', u32(0))
    product = child(tree, 'product')
    for key, value in (
        ('bluetooth-le', u32(1)),
        ('ui-pip', b''),
        ('ui-pinned-app', b''),
        ('ui-floating-live-app', b''),
        ('ui-overlay-app', b''),
        ('hearingaid-audio-equalization', u32(1)),
        ('hearingaid-low-energy-audio', u32(1)),
        ('iap2-protocol-supported', u32(1)),
        ('allow-32bit-apps', u32(1)),
        ('partition-style', b'iOS\0'),
        ('ephemeral-data-mode', u32(0)),
        ('multiuser-sessions', u32(0)),
        ('has-applelpm', u32(0)),
        ('has-boot-chime', u32(0)),
        ('udid-version', u32(1)),
        ('chrome-identifier', b'com.apple.dt.devicekit.chrome.tablet\0'),
        ('device-color-policy', u32(0)),
        ('itunes-min-ver', u32(0xC0500)),
        ('mobiledevice-min-ver', b'1600\0'),
        ('ptp-large-files', u32(1)),
        ('RF-exposure-separation-distance', u32(5)),
        ('disable-globe-map', b''),
        ('sandman-support', u32(1)),
        ('supports-avatars', b''),
        ('wifi-chipset', b'4355\0'),
    ):
        put(product, key, value)
    # MobileGestalt's volume-buttons answer is whether /buttons has function-button_volup; nothing here resolves the
    # function (SwitchButtons owns the buttons; AppleM68Buttons, which would, is not prelinked). J120's bytes.
    buttons = child(tree, 'buttons')
    put(buttons, 'function-button_volup', bytes.fromhex('240000004f4950472800000000000000'))
    put(buttons, 'function-button_voldown', bytes.fromhex('240000004f4950472b00000000000000'))
    maps = child(product, 'maps')
    put(maps, '3d-imagery', u32(0))
    put(maps, '3d-maps', u32(0))


def _boot_slots(tree: list, chosen: list, overlay_dirs: Sequence[tuple[str, str]]) -> None:
    # Directories the kernel shows the Data overlay over: NUL-terminated "<directory>[:<boot-arg>]".
    if overlay_dirs:
        put(
            chosen,
            'switch-overlay',
            b''.join(f'{d}:{g}\0'.encode() if g else f'{d}\0'.encode() for d, g in overlay_dirs),
        )
    buttons = child(tree, 'buttons')
    for key, value in (
        ('compatible', b'buttons\0'),
        ('device_type', b'buttons\0'),
        ('button-names', b'hold\0voldown\0volup\0'),
        ('home-button-type', u32(2)),
    ):
        put(buttons, key, value)
    # APFS's no-SEP mode; it also permits the unencrypted Data volume.
    filesystems = child(tree, 'filesystems')
    put(filesystems, 'DoNotUseSEP', u32(1))
    # Lock timeouts 40 times XNU's 0.5 s, as UniXNU-qemu boots with (machine.c: ml-timeout-lock-panic-scale's ADT form).
    put(child(chosen, 'machine-timeouts'), 'lock-panic-scale', u32(40))
    # Pre-kernel boot timing (boot/loader/stamps.h): zeros here, filled by the loader, logged once by XNU.
    put(chosen, STAMPS_PROPERTY, bytes(STAMPS_BYTES))
    # Per-boot seed report (boot/loader/seed.h): zeros here; the loader mixes SE2's DRBG into /chosen/random-seed
    # and says how it went, XNU logs it once.
    put(chosen, SEED_SOURCE_PROPERTY, bytes(SEED_SOURCE_BYTES))


def ipad_adt(
    data: bytes,
    *,
    smp: bool = False,
    overlay_dirs: Sequence[tuple[str, str]] = (),
    bt_patch: bytes | None = None,
    portrait: bool = False,
) -> bytes:
    """Return the inherited Switch tree plus what iPadOS userland expects of a J120.

    The values are those of UniXNU-qemu's make-qemu-adt.py, which boots this build to SpringBoard.
    """
    tree, end = adt_parse(data)
    if end != len(data):
        raise SystemExit('trailing bytes after the ADT')
    _identity(tree)
    chosen = _chosen(tree, portrait)
    # IOPMrootDomain reads this at boot complete (gSleepDisabledFlag) and refuses every sleep while it is 1. Whether
    # the console may sleep is SwitchSuspend's call at runtime (a CPU power assertion while it cannot).
    put(child(tree, 'defaults'), 'sleep-disabled', u32(0))
    # Both kernels read vm_compressor_mode from here. Without it xnu-10063's CONFIG_FREEZE default (8) leaves the
    # compressor present but inactive: anonymous memory is never compressed and jetsam kills instead. 2 is
    # VM_PAGER_COMPRESSOR_NO_SWAP: the active in-core compressor, no swap files (4 would swap to the SD card).
    put(child(tree, 'defaults'), 'kern.vm_compressor', u32(2))
    _product(tree)
    _radios(tree, bt_patch)
    # The platform UUID. IOPlatformExpertDevice::generatePlatformUUID makes IOPlatformUUID from /chosen
    # unique-chip-id (8 bytes) and chip-id (4 bytes), as iBoot sets them; without them the IOPlatformUUID resource
    # is never published and gethostuuid() with no timeout waits for ever. configd's IPConfiguration start() makes
    # one call (its CGA host UUID, as net.inet6.send.opmode is 1), so configd's plugin thread stops there: no other
    # start(), no prime(), so State:/Network/Interface never lists en0 and wifid makes no Wi-Fi device. chip-id is
    # J120's A10X (T8011); unique-chip-id stands in for the ECID, a fixed value with the overlay MACs' 02:1d:5a.
    put(chosen, 'chip-id', u32(0x8011))
    put(chosen, 'unique-chip-id', struct.pack('<Q', 0x001D5A0000000001))
    _usb(tree)
    _media_accel(tree)
    _capabilities(tree)
    _boot_slots(tree, chosen, overlay_dirs)
    if smp:
        four_cores(tree)
    return adt_serialize(tree)


def with_a64x_cache_pretranslated(data: bytes, cache: bytes) -> bytes:
    """Return the ADT with iOS 27's /chosen/a64x-cache-pretranslated.

    The cdhashes (20 bytes each) of the dyld cache files translated offline, which the kernel maps as they are instead
    of translating them at load (bsd/kern/a64x_xnu.c).
    """
    if not cache or len(cache) % 20:
        raise SystemExit(f'a64x-cache-pretranslated: {len(cache)} bytes, not a list of cdhashes')
    tree, _ = adt_parse(data)
    put(child(tree, 'chosen'), 'a64x-cache-pretranslated', cache)
    return adt_serialize(tree)


def with_switch_pretranslated_trust(data: bytes, module: bytes) -> bytes:
    """Return the ADT with iOS 27's /chosen/switch-pretranslated-trust.

    That is the UUID of the trust module of the System volume's translated executables (the os role's), which the
    kernel's lookup redirect requires loaded and checks each such executable's CDHash against
    (bsd/vfs/vfs_switch_pretranslated.c).
    """
    if len(module) < 24 or struct.unpack_from('<I', module)[0] != 2:
        raise SystemExit(f'switch-pretranslated-trust: {len(module)} bytes, not a version 2 trust module')
    tree, _ = adt_parse(data)
    put(child(tree, 'chosen'), 'switch-pretranslated-trust', module[4:20])
    return adt_serialize(tree)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('adt', 'overlay-plan', 'bt-patch', 'portrait-header', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--smp', action='store_true')
    parser.add_argument('--a64x-cache-pretranslated', type=Path)
    parser.add_argument('--switch-pretranslated-trust', type=Path)
    args = parser.parse_args()
    dirs = [tuple(d) for d in json.loads(args.overlay_plan.read_text())['dirs']]
    adt = ipad_adt(
        args.adt.read_bytes(),
        smp=args.smp,
        overlay_dirs=dirs,
        bt_patch=args.bt_patch.read_bytes(),
        portrait=portrait_native(args.portrait_header),
    )
    if args.a64x_cache_pretranslated:
        adt = with_a64x_cache_pretranslated(adt, args.a64x_cache_pretranslated.read_bytes())
    if args.switch_pretranslated_trust:
        adt = with_switch_pretranslated_trust(adt, args.switch_pretranslated_trust.read_bytes())
    if ('/filesystems', 'DoNotUseSEP') not in parse_adt(adt).properties:
        raise SystemExit('ADT patch failed')
    args.out.write_bytes(adt)


if __name__ == '__main__':
    main()
