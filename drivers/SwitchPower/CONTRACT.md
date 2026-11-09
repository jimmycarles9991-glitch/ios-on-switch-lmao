# thermalmonitord (CLTM) on the Switch: the contract

iPadOS 17.7.11 (21H461) runs as iPad7,1 / J120AP. Its thermal daemon, `/usr/libexec/thermalmonitord`
(ThermalMonitor-1952.120.7), reads every temperature sensor of the J120AP thermal model every 5 s, drives CLTM's
mitigation from them, and reports its health to watchdogd. Before r15-thermal none of its sensors existed, so it
reported unhealthy from boot (`current 0, mask 3fffffffff, expected 3fffffffff ... Missing sensor(s): TG0B ...`) and
watchdogd killed it at 60 s and 120 s and panicked at ~199 s (r7t, r7u).

**[V]** = verified (decompiled/disassembled 21H461 code, the plist itself, IOHIDFamily-2031.120.4 kernel source, or
device logs). **[I]** = inferred (consistent with everything seen, not traced end to end). **[HW]** = needs the next
boot to confirm.

Sources: `thermalmonitord` and `System/Library/ThermalMonitor/*.plist` extracted from the 21H461 root image
(`bundles/ipados-17.7.11-tcg/disks/os.raw`), decompiled with Ghidra 11 headless; class/selector names from
`ipsw macho info --objc`; IOHIDFamily-2031.120.4 (out/agent-scratch/repos/IOHIDFamily, the 21H461 kext's version);
L4T `CTCaer/switch-l4t-kernel-4.9` 2d0059f (`drivers/thermal/tegra/`); device logs
`out/switch/device-logs/boot-1105-1224/boot.logarchive` (r7u, boot 12:19:55). Work files:
`out/agent-scratch/r15-thermal/` (extract/, tmd.ghidra.c, tmd-objc.txt).

## 1. Where the expected sensors come from

- **[V]** `_thermalMonitor_init`: `sysctlbyname("hw.model")` = `J120AP`; the model is
  `/System/Library/ThermalMonitor/` + model + `-Info.plist`. A model containing `DEV` (a dev board) turns the
  watchdog health check off and fakes the battery sensor; ours does not.
- **[V]** The product class is chosen by a hash of the model string (even characters add, odd ones multiply):
  `J120AP` = 0xd77850 -> `tm2c2215485370d730a0de95e9234264e9` (a `CommonProduct` that only overrides
  `updateAllThermalLoad:` and `updateCoreAnalyticsInfo`).
- **[V]** `J120AP-Info.plist` `Sensors`: 43 entries. 38 have `synthSensor` false and are HID sensors; 5 are virtual
  (SS0F, SS0C, TSBH SoCSkin, TS0A Ambient, TSFD CGCenterSkin), computed by thermalmonitord, with
  `requiresWatchDog` false.
- **[V]** `-[HidSensors loadProductTemperatureParameters:]`: `requiresWatchDog` defaults to true, so the watchdog
  mask has one bit per entry index 0..37 = **0x3fffffffff**. `infoOnly` sensors (TP1d, TP3d-TP5d, TP7d, TR1d, TR2d,
  TP0j, TQ0B, every `Th*i`/`Tc*i`) are not read unless info-only sensors are active (a -127 C placeholder is stored
  instead), but **their service must still be matched**: the bit is set for every sensor whose service exists.

## 2. How a sensor is found and read

- **[V]** `-[HidSensors createHIDEventSystemObject]`: `IOHIDEventSystemClientCreate`, matching
  `{PrimaryUsagePage 0xff00, PrimaryUsage 5}`, `{0xff08, 1}` (power sensors) and `{0x20, 0x31}`;
  `copySensorArray` keeps services with page 0x20/0xff08/0xff00 and usage 1/5/0x31. None: `no HID services (5)` /
  `Could not create HIDArray (will retry)` (exactly what r7u logged every 5 s).
- **[V]** Matching a service to a sensor (`FUN_10000fcb8`): the service's `Product` (CFString) equal to the sensor's
  `matchName`; otherwise, for a 4-character key, the service's `LocationID` equal to the 4CC as a big-endian integer.
- **[V]** The read loop is a 5 s `CFRunLoopTimer` (`FUN_100005b10`) calling `-[HidSensors getHIDSensorDict]`: for each
  matched service, first the SMC (`getTemperatureFromSMCForKey:`; no AppleSMC here, so -1), then
  **`IOHIDServiceClientCopyEvent(service, 15 /* kIOHIDEventTypeTemperature */, 0, 0)`** (25 for power sensors) and
  **`IOHIDEventGetFloatValue(event, 0xf0000 /* kIOHIDEventFieldTemperatureLevel */)`**, degrees C as a float, times
  100 truncated to an int. No event: `Could not get event from service (%@)`. Valid only if
  `(uint32)(value - 15000) > 0xffffb5c8`, i.e. -40 C < t < 150 C (`isTempSensorDataValid:value:`). Event timestamps
  matter only for `staleDefault` sensors (none on J120AP).
- **[V]** A per-sensor IIR filter (`filterParams` = time constant in 5 s samples; `filterParams_INTMIN`: the first
  sample initialises it), then `FUN_10000613c` sets the sensor's bit in `current`. After a full pass,
  `(mask & current) == expected` logs `sensors: ...`; otherwise every 4th pass `partial sensors: count %d, current
  %llx, mask %llx, expected %llx` and the service list is rebuilt (`resetHIDSensorDictionary` releases the cached
  services array, +0x280, so the next pass copies the services again). So a list taken while services were still being
  published recovers within 4 passes (20 s), inside watchdogd's 60 s; SwitchSensors starts right after SwitchBattery in
  the board thread to make that rare.
- **[V]** PMU tcal (TP0Z): `calibrationResistorSensor` = TP0Z's index; offset = `calibrationResistorTempDefault`
  (CommonProduct +0x24, **5182 = 51.82 C** in both 21H461 and 24B119, set in `initProduct`) minus the filtered tcal,
  added to every `TP?d`/`TP?[A-Z]` sensor (`fourCharCodeNeedsPMUtcal:`). With no `requiredCalibrationResistorSamples`
  (J120AP) the filter is seeded with the default and then follows TP0Z (time constant 6000 samples); J181AP averages the
  first 5 readings. So TP0Z answers exactly 51.82 C, the "no correction" reference. Until r116 it answered 0.00 C:
  J181 added 51.82 C to every board thermistor from its first pass (TSFD 83.68 C at boot 15's start, TSBH past
  SoCLowHS's 43 C within 10 minutes, then backlight and charge caps and thermal pressure moderate), and J120's offset
  crept toward the same over hours (`test/switch_thermal_tcal_test.cpp`).
- **[V]** J120AP's model (`tm2c22...updateAllThermalLoad:`, constants read from the binary's `__TEXT,__const`), in
  centi-degrees: **SoCSkin TSBH = 68.76 + 0.57 TG0B + 0.42 TP8d**; Ambient TS0A = 155.88 + 1.14 TP2d - 0.29 TP8d;
  CGCenterSkin TSFD = TG0B. Hotspots (plist): `DieTempHS` target 85 C on `dieTempMaxAverage` (the highest of the
  `Th*a`/`Tc*a` "avg die" set), `SoCHighHS` 49 C and `SoCLowHS` 43 C on TSBH (PI loops, kp/ki in the plist), each
  feeding the decision tables (`HotSpotDecisionTableNormal1..3`: dtCPU, dtGPU, dtBacklight, dtCharger, dtRadio,
  dtRefreshRate, dtThermalLevel).
- **[I]** The userspace path: `IOHIDServiceClientCopyEvent` -> backboardd's IOHIDEventSystem -> the kernel service's
  `IOHIDEventServiceUserClient::_copyEvent` -> `IOHIDEventService::copyEvent(type, matching, options)`. The kernel half
  is **[V]** (IOHIDFamily-2031.120.4 `IOHIDEventServiceUserClient.cpp:419-481`); the userspace plugin's
  `IOConnectCallMethod` is not in the sources we have.
- **[V]** `IOHIDEventDriver::copyEvent` (IOHIDFamily `IOHIDEventDriver.cpp:1675`) answers keyboard, orientation and
  vendor-defined events only; the base `IOHIDEventService::copyEvent` returns NULL. So a temperature service needs its
  own copyEvent.

## 3. The health string ("SD: 1 BC: 0 RC: -1 BS: 0")

**[V]** `getalive_func` (the `com.apple.thermalmonitord.watchdog` endpoint): unhealthy iff health checks are on
(`DAT_1000a0710 == 1`: a thermal model was found and the model is not a DEV board) and `(mask & current) != expected`;
`current` is cleared after every check, so every sensor must answer at least once between two of watchdogd's checks.
Results within 10 s of a system wake are ignored (`kern.wake_abs_time` / `IOPMGetLastWakeTime`; ours answers
`0xe00002d8`, logged `Unable to get last wake time`). Only the mask decides; the rest is diagnostics:

| Field | Meaning | Source |
|---|---|---|
| SD | sensor data valid: every retrieved sensor gave a valid value in the last pass | `-[HidSensors isSensorDataValid]` (+0x278) |
| BC | the IOReport `Battery` channel group probe | `FUN_100035c28` (`IOReportCopyChannelsInGroup("Battery")`) |
| RC | the gas gauge reset count, SMC key `GGRC` >> 24; -1 without an SMC | `FUN_100035f6c` |
| BS | battery authenticity: `AppleAuthCPRelay` under `IOPMPowerSource`, `isTrustedForUI` (0 unknown, 1 trusted, 2 not) | `FUN_100035f98` |

## 4. What the Switch publishes (the registry shape)

**[V by source; HW]** One `SwitchHIDDevice` (drivers/SwitchTouch, the original IOHIDDevice through NativeHidInput's
bindings) per HID sensor, 38 in all, under IOResources: VendorID 0x057e, ProductID 0x5108, Transport `I2C`, Built-In,
**Product = the sensor's matchName**, **LocationID = its 4CC** (big-endian), report descriptor Apple vendor 0xff00 /
TemperatureSensor 0x0005 with one Sensor-page Report Interval feature (the ALS's shape, so IOHIDEventDriver starts:
`parseSensorPropertyElement`). IOHIDDevice -> IOHIDInterface -> **IOHIDEventDriver** (our personality, matched by
VID/PID/usage pair), which as an IOHIDEventService publishes PrimaryUsagePage 0xff00, PrimaryUsage 5 and Product.
All 38 share one work loop.

copyEvent: each of these event drivers gets one shared copy of IOHIDEventDriver's vtable whose slot 201
(`IOHIDEventService::copyEvent`, reserved slot 11, declared right after `dispatchEvent` 198 / `getPrimaryUsagePage`
199 / `getPrimaryUsage` 200; 198 is the edge wrapper's, proven on hardware) answers type 15 from the sensor's source and
passes every other type to the original. Installed only after slots 199/200 answered 0xff00/0x0005 on that driver
(otherwise one log line and the sensor stays unhooked), by whichever comes first: IOKit's first-publish notification
for IOHIDEventDrivers (any driver; the callback keeps those whose provider is a temperature device), or the hook pass,
a one-shot thread call queued once the 38 devices exist that waits for each device's matching (`IOService::waitQuiet`,
at most 10 s for all of them) and hooks the IOHIDEventDriver that has opened its interface. The watchdog's verdict
(section 7) hooks any driver that matched later. r8a-r8e booted with the notification matching an IOPropertyMatch on the
driver's VendorID/ProductID and no hook pass: every event driver started and was published, thermalmonitord found all
38 services, and none was hooked (`0 of 38 HID sensors hooked`; thermalmonitord: 21 `Could not get event from service`
per pass and `partial sensors: ... current 9249276ba`, exactly the 17 infoOnly bits). One line says how they were
hooked: `SwitchSensors: 38 of 38 HID sensors hooked (P by the publish notification, Q once matching was quiet)`. The
event is an `IOHIDEvent` from its metaclass, `setType(15)` (= initWithType), `setTimeStamp` (the measurement's time),
`setFixedValue(0xf0000, degrees 16.16)`; the first one is read back (`getType`, `getFixedValue`) and a mismatch turns
temperature events off for the boot with one line.

## 5. The mapping: every expected sensor and its Switch source

The Switch (T210B01 "Mariko", OLED) measures temperature at: SOC_THERM's six on-die tsensors (**cpu0-cpu3**, one beside
each A57 core; **gpu**; **pllx**, mid-die at the CPU PLL; T210B01 dropped T210's mem0/mem1), the **TMP451** (remote: the
SoC's thermal diode, with hekate's -12.5 C offset; local: the PCB at the sensor, next to the SoC), and the **MAX17050**
fuel gauge's battery-pack thermistor. The BQ24193 charger (REG08 THERM_STAT, REG09 NTC fault bits), the
MAX77620/MAX77812 PMICs (TJALRM 120/140 C alarm bits) and the BM92T36 USB-PD controller have **no temperature
readout**, so they back no sensor (making one up from an alarm bit would invent a value).

| # | 4CC | matchName | J120AP use | Switch source | Why this is the nearest point |
|---|---|---|---|---|---|
| 0 | TG0B | gas gauge battery | SoCSkin 0.57, CGCenterSkin | MAX17050 TEMP | the same thing: the battery cell, by its gauge |
| 1-8 | TP1d..TP8d | PMU tdev1..8 | TP8d SoCSkin 0.42 (filter 45), TP2d Ambient; others info | TMP451 local (PCB) | board NTCs; the PCB channel is the Switch's one board-level point away from the cells |
| 9-10 | TR1d, TR2d | PMU2 tdev1..2 | info | TMP451 local (PCB) | as above |
| 11 | TP0Z | PMU tcal | tcal offset | the reference: 51.82 C | the PMU ADC's calibration channel; the Switch's sensors are digital and need no correction, and thermalmonitord's own default is exactly "no correction" |
| 12 | TP0j | PMU tjunc | info | TMP451 local (PCB) | the PMIC die has no readout; the PCB beside it is the nearest measured point |
| 13 | TQ0B | Charger tbat | info | MAX17050 TEMP | the battery cell (the BQ24193 only compares the pack NTC against its windows) |
| 14-16 | Th0i/a/x | PMGR SOC Die Temp Sensor0 | die avg/max, lifetime servo | SOC_THERM gpu | on-die, outside the CPU cores; GPU block |
| 17-19 | Th1i/a/x | ... Sensor1 | " | SOC_THERM pllx | on-die, mid-die |
| 20-22 | Th2i/a/x | ... Sensor2 | " | TMP451 remote (SoC diode) | on-die thermal diode |
| 23-25 | Th3i/a/x | ... Sensor3 | " | SOC_THERM gpu | T210B01 has three die points outside the cores; the fourth slot takes the GPU, the largest block |
| 26-37 | Tc0..3 i/a/x | ACC Temp Sensor0..3 | die avg/max, lifetime servo | SOC_THERM cpu0..cpu3 | ACC = the CPU complex: core n's own tsensor |

i/a/x on Apple's PMGR are instantaneous / window average / window maximum. The Switch's sensors give one instantaneous
reading; thermalmonitord reads all three in the same pass, so the one-sample average and maximum of that pass are that
reading. Consequences to know: TS0A "Ambient" reads high (it is J120's formula over the PCB, ~0.85 x PCB); SoCSkin is
0.57 x battery + 0.42 x PCB, so SoCLowHS (43 C) starts mitigating with e.g. battery 36 C + PCB 52 C; DieTempHS (85 C)
watches the hottest of cpu0-3, gpu, pllx and the SoC diode.

How each is read, on thermalmonitord's own 5 s cadence (it calls in; no timer of ours):

- **SOC_THERM** (`SwitchSensors.cpp`): brought up once, as L4T `soctherm.c` for `nvidia,tegra210b01-soctherm`:
  fuse clock visible; per-sensor calibration from FUSE_TSENSOR_COMMON (0x180) and each sensor's calibration fuse
  (`tsensor-fuse.c`, T210B01 config tall 16300, tsample 240, pdiv 12, pdiv_ate 6, tsample_ate 480; alpha/beta from
  `tegra210b01_tsensors`); SOC_THERM (peripheral 78) under reset, clock 51 MHz from PLLP (CLK_SOURCE_SOC_THERM
  0x644 = 0x4000000e), TSENSOR (peripheral 100) 400 kHz from CLK_M (0x3b8 = 0x8000005e), out of reset; CONFIG0/1/2 per
  sensor, SENSOR_PDIV and HOTSPOT_OFF (cpu 10, gpu 5). A read is `TS_x_STATUS1` (valid bit 31, readback 15:0 =
  degrees, +0.5, negate) at the moment of the call. Once up, SwitchSocTherm.cpp keeps a one-degree LVL0 window on the
  CPU, GPU and PLLX groups (THERMCTL_INTR, INTID 80). Past L4T's software limits (SoC diode 92.5 C, PLLX 94.5 C,
  SwitchThermalLimits.h) SwitchThermal shuts the console down in order with a record in the RTC that the next boot files
  (SwitchResetCause.h); THERMTRIP with the PMC's reset (PLLX 96 C, CPU and GPU 127 C) is only the backstop above that.
  No hardware throttle is programmed.
- **TMP451**: SwitchThermal's last good reading, taken on every ALERT (the window around the fan levels and the DFLL's
  limits) and every time a SOC_THERM group moves a degree; it fails only once SwitchThermal has lost the sensor.
  SwitchThermal stays the chip's only reader.
- **MAX17050**: SwitchBattery's last reading, kept current by the gauge's **temperature alert window** (TAlrtTh, one
  degree either side of the last reading rounded; ALRT on PY0, which SwitchBattery already handles), besides its other
  alerts and the 120 s re-read. Before r15 the window was wide open.

A source that cannot answer makes that read fail (no event); thermalmonitord then reports the sensor missing. Nothing
is substituted.

## 6. Mitigation: where CLTM's decisions go

- **[V] CPU**: `-[MitigationController updateCPU]` (no power zones on J120AP) writes four DVD factor contributors on
  **`IOServiceMatching("ApplePMGR")`** with `IORegistryEntrySetCFProperty`:
  `AppleARMPerformanceControllerDVDFactor1-slowloop`, `-hip`, `-upo`, `-lpm`, each
  `value < 100 ? value * 65536 / 100 : 0x10000` (a 16.16 share; `cpuComponentControl` maps max load index to DVD factor:
  2->0 ... 37->35 ... 100->100). It also looks for `AppleCLPC` (absent: `Could not find CLPC service`). Here `ApplePMGR`
  is ShengPlatformPower (a subclass of the original; it grants thermalmonitord its property writes with
  NativeSandboxGrantProcess, since the sandbox profiles name only original classes); its new `setProperties` takes the
  four keys (and publishes them), and the lowest becomes **`switch_perf_cltm_ceiling()`**: the CPU governor's highest
  point is also at or below that share of 1785 MHz (`switch_gov_index_at_fraction`), together with the DFLL's thermal
  cap, at once and also across governor restarts. **[I]** That a DVD factor is a share of the top clock is our reading
  (CLPC's side is not in 21H461's userspace); the keys, scaling and target service are **[V]**. One os_log line per
  change. In PLL or fixed-DFLL mode the ceiling has no effect (the line says so): that includes main's current default
  (c1e0a4b, r7z: the DFLL fixed at 1785 MHz with the governor off while r7y's slowdown is bisected). Once the governor
  is the default again, CLTM's cap applies with no further change; until then the CPU's thermal protection is
  SwitchThermal's DFLL voltage cap (64/84 C) alone.
- **[V] GPU**: `-[MitigationController updateGPU]` writes `GPUMaxPower` (percent) on
  **`IOServiceMatching("AppleDieTempController")`**, which does not exist here (`Cannot find AppleDieTempController
  service`), so GPU caps reach nothing yet. Next step (needs r15-dvfs's GPU governor on main, and a decision on
  publishing a stand-in of that class name, which LifetimeServoController also looks for): a service accepting
  `GPUMaxPower` that clamps TegraGM20B's governor ceiling.
- **[V] Thermal pressure**: ThermalLevelCC -> `OSThermalNotification` / `com.apple.system.thermalpressurelevel`;
  already delivered (r7u: backupd, audiomxd and mediaserverd received level 0). With real sensors the level now
  follows dtThermalLevel.
- **[V]** Other components (backlight, charger, radio, refresh rate) act through their own userspace services;
  unchanged here.
- SwitchThermal's fan curve and its CPU/DFLL trips are independent of iOS and stay the hardware safety net.

## 7. The userspace watchdog

NativeSwitchWatchdog's stopgap (`off_ = true` at start) is gone. watchdogd's first open of the IOWatchdog waits (at
most 180 s, woken whenever a source changes, a driver is hooked or a sensor is first read; no polling) for
`switch_sensors_wait_ready()`: all 38 HID services hooked, **thermalmonitord has read each of the 21 sensors it reads
(the non-infoOnly ones) through the hook at least once** (end to end: the hook returned an event to a client's
copyEvent), every SOC_THERM sensor valid, a TMP451 and a battery reading, temperature events working. Then monitoring
stays on and the kernel logs `SwitchSensors: thermalmonitord's 38 sensors answer (it has read all 21 it reads): CPU ...
C`. The wait is long because thermalmonitord's first pass is late on a slow boot: r8e's watchdogd opened at 38 s of
uptime, thermalmonitord's first pass came at 90 s, the boot logo at 137 s. Nothing is monitored during the wait.
Watchdog timing against the slow boot: from the verdict, watchdogd pings each monitored service every 10 s, kills one
silent for 60 s and panics at the third; r8e's SpringBoard main thread never went 6 s without a log line between its
first line and the logo, and the pre-sensor boots with monitoring on (r7t, r7u) killed only thermalmonitord. Otherwise
monitoring is
turned off before watchdogd asks (method 0), exactly as after a watchdogd crash: one line
`SwitchWatchdog: userspace watchdog off for this boot: thermalmonitord's sensors do not all answer (<what>)`,
watchdogd reads "disabled" and exits 0, and nothing can end in a watchdog panic that boot.

A source that answered at boot and stops later logs `SwitchSensors: <source> stopped answering: thermalmonitord reports
its sensors missing` (and `... answers again`). A source that stays dead for 60 s then gets thermalmonitord killed and,
after 120 s more, the watchdog panic, as on an iPad (watchdogd panics through its own user client; the kernel cannot
recall that once monitoring is on). SwitchThermal retries a failed TMP451 every second and the fan runs at full speed
meanwhile. SC7 resets SOC_THERM: SwitchSuspend's wake programs it again (`switch_sensors_resume`) right after the
thermal service, among the first wake steps.

## 8. Next-boot checks

From the Mac (device on USB): `idevicesyslog -u 166bbe821dd2f22e91eab155cfc9355e1a02593f archive
r15.tar --age-limit 600`, then `/usr/bin/log show --archive <dir>.logarchive --style compact --info --debug`.

1. Kernel: `SwitchSensors: SOC_THERM up (fuse common ...; CONFIG2 ...)` and no `SwitchSensors: ... off` /
   `SwitchHID: temperature ...` line.
2. Kernel: `SwitchSensors: 38 of 38 HID sensors hooked (P by the publish notification, Q once matching was quiet)`, then

   ```text
   SwitchSensors: thermalmonitord's 38 sensors answer (it has read all 21 it reads): CPU a b c d, GPU e, PLLX f, SoC diode g, PCB h, battery i C
   ```

   with plausible values (CPU/GPU/PLLX within a few degrees of the SoC diode at idle, PCB below them, battery near room
   temperature), and **no** `userspace watchdog off for this boot`.
3. Kernel: `SwitchWatchdog: watchdogd connected; userspace watchdog armed`.
4. thermalmonitord: no `no HID services (5)`, no `found error sensor result`, no `partial sensors`, no
   `Could not get event from service`.
5. watchdogd: no `is_alive_func returned unhealthy` for thermalmonitord; no thermalmonitord crash at 60/120 s (no new
   `thermalmonitord-*.ips` in mobile/Library/Logs/CrashReporter) and no panic at ~199 s. Uptime past 5 minutes.
6. ioreg: `ioreg -l -w0 | grep -A3 '"Product" = "PMU tdev8"'` shows PrimaryUsagePage 65280, PrimaryUsage 5.
7. Load (a game or a long Safari scroll for 5 min): the SoC diode and cpuN rise together; with SoCSkin over 43 C
   thermalmonitord's decisions show up as `ShengPlatformPower: thermalmonitord CPU ceiling NN%` os_log lines and the
   periodic `SwitchPerf governor:` line's range top drops; `com.apple.system.thermalpressurelevel` goes above 0 under
   sustained heat.

## 9. Exact user test

1. Deploy as usual (`bazel run //deploy:sd -- diskN --yes --stage-gpu`), boot, unlock.
2. Leave it on the home screen for 5 minutes (past 60 s, 120 s and 199 s). It must not panic or reboot.
3. Pull the log: `idevicesyslog -u 166bbe821dd2f22e91eab155cfc9355e1a02593f archive r15.tar --age-limit 600`, and run
   `log show ... | grep -E "SwitchSensors|Switch watchdog|is_alive_func|thermalmonitord.*(error sensor|partial|no HID)"`:
   expect the two SwitchSensors lines and "watchdogd connected; userspace watchdog armed", and nothing else from that
   pattern.
4. Then play something heavy for 10 minutes (or loop a 4K video plus a browser): the console stays usable (the CPU
   governor's top may come down: that is CLTM working), the fan follows SwitchThermal's curve, no panic.

## 10. iOS 27 (24B119) on J181AP

24B119's thermalmonitord, booted as J181, reads `/System/Library/ThermalMonitor/J181AP-Info.plist`; SwitchSensors
publishes that model's services when the kernel is built for 24B119 (`SwitchSensorModels.h`, selected by
`SWITCH_IPSW_DEVICE_J181AP`), and J120AP's for 21H461.

- **[V]** `Sensors`: 53 entries, 6 synth (TK0F, TK0C, TK1C, TK2C, TSFD, TSBH; `requiresWatchDog` false), 47 HID, so
  the watchdog mask is **0x7fffffffffff**. 13 of the 47 are `shadowSensor` (`Th0f` and the like): the `Avg:` reading
  again under its own filter.
- **[V]** `-[HidSensors loadProductTemperatureParameters:]` keeps the **matchNames** as its HID keys (the 4CCs go to a
  separate list); `getHIDSensorDict` maps each service to the first key whose matchName equals its Product (or, for a
  4-character matchName, whose 4CC equals its LocationID), stores readings by key, and expects
  `keys - shadows` services: **34**, one per distinct matchName. Shadows take their key's reading.
- **[V]** The read path is 21H461's: `SensorDispatcherHelper getTemperatureFromSMCForKey:` first (-1 with no
  AppleSMCSensorDispatcher key, so on to HID), then `IOHIDServiceClientCopyEvent(service, 15)` and field `0xf0000`.
- **[V]** J181AP's model (24B119 `-[tmf02fbce43bb83f6eb51bd31374aa8279 updateAllThermalLoad:]`, the class for
  model hash 0xdc50f0), in centi-degrees: **BCH7Skin TSBH = 0.88 TG0B + 0.13 TP3d - 22**; CGCenterSkin TSFD = 67 +
  1.06 TP5d - 0.08 TP2d (pushed to the display driver). SoCHighHS (49 C) and SoCLowHS (43 C) act on TSBH, DieTempHS
  (85 C) on `dieTempMaxAverage`; their decision tables cap the backlight, the charge rate (`ChargeLimitTable`), package
  power and the thermal level.
- **[V]** The J120AP table under J181 matched only `PMU tcal` and the PMGR die sensors 0-2: thermalmonitord would have
  reported the rest missing and NativeSwitchWatchdog turned the watchdog off for the boot.
- **Open [V by strings]**: J181's plist asks for CPMS package control (`packageComponentControl.expectsCPMSSupport`,
  `powerZoneParams.usesPackageControl`), fast die control, the lifetime servo and power-zone telemetry. 24B119 looks
  for `ApplePPM` (CPMS), `AppleCLPC` and `AppleDieTempController` for those, none of which the Switch publishes, so
  CPU and GPU mitigation from CLTM reach nothing on iOS 27 until stand-ins exist; SwitchThermal's fan curve and DFLL
  caps remain the protection.
