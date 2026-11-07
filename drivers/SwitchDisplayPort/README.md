# SwitchDisplayPort

TV output over the USB-C port, the way Nintendo's dock does it. The BM92T36 negotiates DisplayPort alternate mode, and
SOR1, driven by display controller B, sends DisplayPort into the dock's DP-to-HDMI converter.

It is off by default. Turn it on with `--//drivers/SwitchDisplayPort:enabled` once USB host mode works on the device.

```sh
bazel build --//drivers/SwitchDisplayPort:enabled //:boot
bazel test //drivers/SwitchDisplayPort/test:all
```

The setting reaches `SwitchPD.cpp` through `defines` on this package's `hdrs`. Kernel compiles only see those once the
kernel toolchain has the `preprocessor_defines` feature (r24-usb3's change to `bazel/toolchains/kernel/config.bzl`).
The alternate-mode engine (`SwitchPDAltMode.cpp`) is always linked, because the dock's hub and USB 3 need it. With the
setting off it skips the DisplayPort steps.

## How it works

- The BM92T36 runs DisplayPort itself. After `DP_CFG_AND_START_HPD` it answers Attention messages by driving HPD on
  PCC1, which the pinmux routes into DPAUX1, so plug, unplug and IRQ_HPD arrive as DPAUX1 interrupts and no Tegra code
  parses Attention messages.
- `SwitchPDAltMode.cpp` uses the controller's COMMAND register only while SwitchPD's own USB-PD request is at rest, that
  is, once the dock's 15 V contract is made. A contract alert while a VDM is out pauses the step, which is sent again
  later. The contract and the charger are left alone throughout.
- When an earlier boot left DisplayPort configured, the engine sends `STOP_HPD_EXIT_DP`. bm92txx resets the controller
  in that case, which drops the contract and the charging with it.
- Pin assignment D (two DisplayPort lanes, one SuperSpeed pair left for USB 3) is taken whenever it is offered, and C
  (four lanes) otherwise. The dock reports a plug but answers like a receptacle and reads the Configure pin assignment
  from bits 23:16, so bm92txx puts it there; every other partner gets the standard bits 15:8.
- The dock's LED and USB hub (`HUB_CONTROL`, retried while the hub is busy) are set up whenever the engine runs, as
  bm92txx does, so USB devices keep working with the TV on.
- To reach the dock's hub the Switch swaps to DFP, so usbmux and `idevicesyslog` over USB stop working in the dock.
  Nintendo's firmware behaves the same way. Only a source with the dock's power signature (two PDOs, the first 5 V
  500 mA; `SwitchChargePolicy.h`) is asked to swap. bm92txx asks any dual-role source, and a Mac that accepted would
  stop being the Switch's USB host. A USB-C to HDMI adapter with pass-through charging is therefore not driven yet.
- Output stays at or below 1080p60 whatever the EDID offers: 1080p60 first, then modes of 50 Hz and up by size, then
  the rest. The link trains only as fast as the mode needs, HBR ×2 for 1080p60 on pin D and RBR ×4 on pin C. When
  training fails at a rate, the next rate down is tried and the mode is picked again.
- SOR1's lane sequencer, power, attach and wake handshakes have no interrupt and complete in microseconds, so each gets
  a bounded 10 ms wait; one that runs out turns DisplayPort off with one log line. AUX transactions end by interrupt,
  and a lost AUX_DONE panics with the GIC's view of INTID 43.
- Display controller B's frame-end interrupt is on only while a flip waits to be taken, so a still screen costs no
  interrupts.
- vdd-hdmi-dp is MAX77620 LDO8 at 1.0 V. It has no FPS slot, so software turns it on and off.
- The pads follow upstream Linux's DP SOR (`TEGRA_IO_PAD_LVDS`, IO_DPD2 bit 25), and the lanes follow upstream's
  tegra210 tables (crossbar 2 1 0 3 4, identity lane map), so two lanes power PADCTL0 bits 0 and 1. Downstream L4T names
  those bits differently (its PD_TXD_0 is bit 2) and which two it sets on the Switch is unverified; check it first on
  the device (test plan step 4).

## iPadOS external display

From static analysis of 21H461. None of this is implemented yet.

- libIOMobileFramebuffer counts the `/arm-io` children whose `device_type` is `display-subsystem` or
  `ext-display-subsystem`, then matches `IOMobileFramebuffer` services and reads the boolean `"external"` property on
  each. A true value makes the display `external-N`, which QuartzCore names `TVOut`; `-[CADisplay isExternal]` is true.
- The node and the service must exist from boot. With fewer services than nodes the library waits up to 30 s, which
  breaks the boot-time target, so the external service is always registered and reports "unplugged" while nothing is
  attached, as Apple's `dispext` does.
- The modes are registry properties, published on plug and removed on unplug: `TimingElements`, `ColorElements`,
  `PreferredTimingElements`, `Transport` = `{Downstream: "DP"}` and optionally `DisplayAttributes`.
- After the type-0 hot-plug notification CA calls user-client selectors 25 GetDigitalOutState (slot 186), 22
  SetDisplayDevice(0) (slot 183), 23 SetDigitalOutMode(colorID, timingID) (slot 185) and SetDisplayDevice(2), then
  swaps as usual.
- This device mirrors only. Stage Manager's extended display needs MobileGestalt `DeviceSupportsEnhancedMultitasking`,
  which iPad7,1 is inferred not to have.

The next step is a second `IOMobileFramebufferLegacy` instance (or an external mode of `NativeLegacyDisplay`) on an
`/arm-io` node with `device_type = "ext-display-subsystem"`, written against the method names r25-xnu27's
`kits/SwitchABI` tables give Apple's slots (they differ between 21H461 and 24B119):

- `"external"` = true before `registerService`, and `isMainDisplay()` false;
- `set_display_device(uint32_t)` accepts 0, 1 and 2; `set_digital_out_mode(colorID, timingID)` picks the mode and has
  the session train for it; `get_digital_out_state(uint32_t *)` returns the plug state; plug and unplug are announced
  with the non-virtual `send_hotplug_message`;
- swaps go to `TegraDisplayB::scanout` through an SMMU domain of the DCB swgroup (MC 0x244, TRANSLATION_ENABLE_0 bits 2,
  4, 6 and 17) with an ASID of its own, ordered after the GPU's rendering as the panel's direct scanout is.

Until this lands, a plugged TV gets the best mode and a black screen, which is enough to prove the link.

## DisplayPort audio (plan, not built)

SOR1 carries audio from the HDA controller (`hda@70030000`) as DP secondary data packets.

1. Linux `tegra_sor_audio_prepare/_enable` writes the SOR side: the HDA codec scratch interrupt
   `SOR_INT_CODEC_SCRATCH0` gives the format, then the audio infoframe and `SOR_AUDIO_*`.
2. An HDA controller and codec driver feeds those samples.
3. On iPadOS, a CoreAudio output follows the TV.

## Device test plan

Run this once host mode works, docked, with a 1080p TV and a 4K TV. Read logs with `idevicesyslog` before docking, since
device mode ends in the dock, and from the SD card's log archive after.

1. Plain charging, with `enabled` off and then on. A USB-PD charger gives no `alternate mode` lines and an unchanged
   `SwitchPD: contract` line. The dock without a TV gives `contract 15 V`, the engine's sequence with no failure line,
   the dock's LED on and the USB hub enumerating with a keyboard on it.
2. In the dock, the 15 V contract and the charging current hold through plugging, unplugging and re-plugging the HDMI
   cable, and the battery keeps charging for 10 minutes.
3. `SwitchDisplayPort: DisplayPort on 2 lanes (pin assignment D …)`. USB 3 still works on the dock's rear port
   (`SwitchUSB: SuperSpeed lane up (pairs free, …)`).
4. Plugging the TV gives `link up at 2700 Mb/s x2` with no clock-recovery failure. If clock recovery fails on one lane,
   check L4T's two-lane PADCTL0 bits first.
5. The 4K TV gives `external screen 1920x1080 at 60.000 Hz`, a 1080p50-only input 1080p50, and a monitor that takes
   only 75 Hz at 1080p its 60 Hz mode.
6. Ten HDMI unplug/replug cycles, then ten dock cycles: one `HPD low` / `link up` pair each, no panic, and the internal
   panel never blinks.
7. Power-cycling the plugged TV gives `the link lost lock … training again` or a clean HPD cycle, then the link back.
8. With the TV on a still screen, `switch-diag` kprof shows no SwitchDisplayPort interrupt rate.
