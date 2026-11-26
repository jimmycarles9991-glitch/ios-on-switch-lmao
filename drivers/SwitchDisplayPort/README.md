# SwitchDisplayPort

The USB-C DisplayPort path is linked into the kernel by default. A native external
`IOMobileFramebufferLegacy` service exists from boot and reports disconnected until
an EDID is available. iPadOS chooses the output mode and presentation behavior.

The BM92T36 negotiates alternate mode while the power contract is stable. Pin D
keeps two DisplayPort lanes and the USB SuperSpeed pair; pin C uses all four lanes.
The partner's advertised assignments decide which can be used. Source-powered
adapters can enter alternate mode when the Switch has the host data role.

The driver publishes progressive RGB8 timings within the link, PLL and display
controller limits. Display controller B scans the compositor's shared IOSurfaces
through its own SMMU domain. Cached mappings survive recurring frames; completed
producer fences allow a direct flip. The external display is always a clone of
the panel. It publishes three layers so that Core Animation keeps detaching video
while docked, and windows A, B and C show the clone's UI over its video. Core
Animation sends a refused swap again every frame, so `validateSwap` refuses only
what the published limits rule out. AUX completion, frame retirement and blocked
GPU admission use interrupts or completion notifications. Suspend tears down the
link and resume probes the sink again.

Audio uses the sink's EDID capabilities. HDA prepares its clocks and reset before
the DP workloop publishes ELD/presence, then discovers the codec. Disconnect stops
the audio path; iPadOS controls routing through CoreAudio.

Dock TV wake uses the STDP2550/RTD2172N
[AUX CEC mailbox](https://github.com/CTCaer/switch-l4t-kernel-nvidia/blob/76e6d48970b451c242c20f298b8d63027836bb0b/drivers/video/tegra/dc/dp_branch.c#L87-L162),
including the [OLED bridge](https://github.com/CTCaer/switch-l4t-kernel-nvidia/commit/679403ef6dba2f77200479cb4ad8c337c76731b4).
ImageViewOn can run before HPD/EDID; ActiveSource requires the real HDMI physical
address and active OS-selected output. The reference supplies no TX-complete
interrupt contract, so each send gets one bounded timer/status read. RX uses HPD
IRQ. Suspend disables the bridge's CEC address before AUX powerdown; HDMI topology
changes allocate a fresh address. Physical detach can remove AUX before cleanup.

```sh
bazel test //drivers/SwitchDisplayPort/test:all
bazel build //:boot
```

Host tests cover PD, EDID, link training, CEC policy/lifecycle and the native IOAV
timing layout from both pinned kernelcaches. OLED dock/TV and direct USB-C display
acceptance remains pending: check both orientations, reconnect, charging under USB
load, audio routing, and sleep/wake. TV standby must also prove AUX access with HPD
low and actual CEC wake. Confirm SuperSpeed when the negotiated pin assignment
leaves its pair free.
