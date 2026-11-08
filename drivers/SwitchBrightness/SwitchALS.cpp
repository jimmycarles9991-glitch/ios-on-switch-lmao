// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Switch OLED's ambient light sensor as iPadOS CoreBrightness's ALS.
//
// Hardware: ROHM BH1730FVC on I2C2 (0x7000C400) at 0x29 (its fixed address, datasheet "0101001"), behind the OLED panel.
//   * The same part, bus and address on every model: Atmosphere i2c_bus_device_map.inc (I2c2 Bh1730 0x29, fast mode,
//     interrupt 116 = SPI 84); switchroot L4T nx-platforms/tegra210-nx-sensor.dtsi i2c@7000c400 bh1730fvc@29, which
//     tegra210b01-fric.dts (Aula) recalibrates for the OLED's window. Nothing else of ours is on I2C2 (TegraI2C.h).
//     Driver: CTCaer/switch-l4t-kernel-4.9 drivers/iio/light/bh1730.c (GPL-2.0, Copyright (c) 2021 CTCaer); maths in
//     SwitchALSMath.h. hekate bdk/input/als.c drives the same part.
//   * Power (L4T tegra210b01-fric-power-tree.dtsi bh1730fvc@29): VCC = vdd_3v3 (MAX77620 GPIO3, FPS-sequenced, on
//     whenever the console is; the SD card's supply), DVI, the I2C I/O reference (VIH 0.7 * DVI) = MAX77620 LDO6 at
//     2.9 V, "Touch Panel, ALS", which SwitchTouch switches with the panel. Horizon enables a 2.9 V regulator for I2C2
//     before using the bus (i2c_bus_accessor.cpp, 560 us), which can only be LDO6: the bus's pull-ups (inferred).
//     So the sequence is L4T's (bh1730_power_ctrl: vdd, vid, 10 ms; then bh1730_power_on/init_config): SwitchTouch
//     turns LDO6 on and tells this driver (switch_als_supply) at least 12 ms later (its own 1 + 1 + 10 ms); only then is
//     I2C2 brought up (pinmux, drive, clock, controller, bus clear: TegraI2C) and the part identified, reset and set
//     up, at the next sample. LDO6 off (screen dark, SC7) stops the sensor first. The datasheet's VCC power-on time
//     (2 ms) passed long before: vdd_3v3 is up from power-on.
//   * No interrupt line. The OLED DT node has no interrupts. The reference design's ALS_PROX_INT pad (PX3) is the
//     VDD_5V3 enable output on Mariko boards (L4T tegra210-nx-power-fixed.dtsi v_vdd5v3 gpio X3, "Only enabled on
//     Modin/Vali/Fric"; Atmosphere gpio_internal_pad_map_combination.inc DeviceCode_Vdd5V3En = Port_X_3), with its
//     input buffer off (tegra210b01-fric-pinmux.dtsi; Atmosphere pinmux_initial_pad_config_aula.inc AlsProxInt 0x4),
//     and Horizon has no GPIO for the sensor at all. L4T, Android, hekate and Horizon all read it on demand.
//   * The clock: the sensor converts continuously on its own ADC clock (102.6 ms, 38 cycles, + 2 ms), and this work
//     loop's timer takes the result every kSampleUS (SwitchALSMath.h) while it samples; the reading needs no frame
//     sync, so the display's FRAME_END interrupt can stay masked while nothing else needs it. It is set up as soon as it
//     may sample (the DC restarted for a wake, LDO6 on), so the first result is one sample period later, while the panel
//     still wakes: CoreBrightness waits only 250 ms after display-on for a first sample and otherwise takes 450 lux
//     (AABC::FirstALSSampleTimeout; a wake on 11-06 lit at 141 nits in a 3 lux room). Samples it takes for stale
//     (stamped before its display-on) or gets with the display off cost nothing. When the panel goes dark or the
//     console sleeps the timer stops, which is when CoreBrightness ignores ALS events anyway ("ALS event arrived when
//     display OFF"). A sensor that does not
//     answer for kErrorLimit samples, a supply that never comes, a part that is not a BH1730 and a HID path nobody
//     takes each log one line and turn the sensor off for the boot.
//   * Seen on hardware: every transfer to 0x29 NACKed with LDO6 on (POK), while TegraI2C's bus clears completed and the
//     controller saw no ACK, so SDA was high to it and SCL ran. GPIO_IN of pins in SFIO mode then read SCL 0 SDA 0,
//     which suggests it does not show them (unverified, TegraGPIO.h). So the give-up logs everything needed in one boot
//     (probe()): the lines read as GPIO inputs next to SFIO-mode GPIO_IN of the working I2C1 and I2C3, the pads now
//     and as the boot chain left them, the PMC's I/O rail power, voltage and deep power down, the controller's last
//     failure (address or data NACK) and a scan of the bus.
//
// iPadOS side (21H461; CoreBrightness, libMobileGestalt and AccessibilitySettings disassembly):
//   * MobileGestalt ambient-light-sensor (lo3szoQ4sLy7o3+ZD0GcAQ) is IOServiceGetMatchingService(IOPropertyMatch
//     {PrimaryUsagePage 0xff00, PrimaryUsage 4}), which the light HID device below answers. Settings (AXDisplayController
//     specifiers, AUTO_BRIGHTNESS) and CoreBrightness (DisplayCreateWithID, DisplaySetProperty: "ALS is not supported on
//     this device -> ignore setting of DisplayBrightnessAuto") both gate on it. The answer is precacheable, so XNU drops
//     MobileGestalt's cache when switch_als_published() changes from the last boot (kern_exec.c).
//   * -[BLControl startHIDSystemClient] matches DeviceUsagePairs {0xff00, 0x0004}; AABC::registerService takes the
//     service only when Built-In is true (SwitchHIDDevice sets it; IOHIDEventService copies it from the interface).
//     Orientation, Placement, ALSSensorType and the integration times are optional (front ALS, defaults).
//   * AABC::HandleALSEvent reads IOHIDEventGetFloatValue(event, 0xc0000) (kIOHIDEventFieldAmbientLightSensorLevel, lux)
//     from kIOHIDEventTypeAmbientLightSensor (12) events; colour only when field 0xc0006 (colour space) is set.
//   * CoreBrightness also sets ReportInterval, ALSIntegrationMode, IOHIDALSContinuousMode and IOHIDBacklightLevel on the
//     service. IOHIDEventDriver forwards only ReportInterval (to the descriptor's Report Interval feature), which sets
//     the sample period here (reportIntervalSet), never shorter than a whole conversion (kSampleUS).
// The HID service is a SwitchHIDDevice (kSwitchHIDLight) and its IOHIDEventDriver. IOHIDEventDriver has no ambient light
// element of its own, so the events are made here and handed to that driver's dispatchEvent (switch_hid_dispatch_event).
//
// IOHIDEvent is IOHIDFamily's class and has no binding here; its factories are statics, but setType is virtual and is
// initWithType: an IOHIDEvent allocated through its metaclass and given setType(12) is the same object
// ambientLightSensorEvent() returns. Slots in the 21H461 IOHIDFamily (vtable 0xfffffff005d07318 in our kernel): 16
// getTimeStamp (ldr x0,[x0,#0x30]), 17 setTimeStamp (str x1,[x0,#0x30]), 18 getType (data->type), 19 setType (b
// initWithType(this, type, 0)), 24 setIntegerValue(key, value, options); _data at +0x10.
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraI2C/TegraI2C.h"
#include "../SwitchTouch/SwitchHID.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "SwitchALSMath.h"
#include "SwitchBrightness.h"
#include "TegraPanel.h"

namespace {
using namespace switch_als;
constexpr unsigned kBus = 2;
constexpr uint8_t kSensor = 0x29;

// BH1730FVC: a command byte 0x80 | register (auto-increment) or 0xe0 | special command (datasheet COMMAND; bh1730.c,
// hekate als.h).
enum : uint8_t {
    RegControl = 0x00,
    RegTiming = 0x01,
    RegGain = 0x07,
    RegId = 0x12,
    RegData0 = 0x14
};

constexpr uint8_t kCommand = 0x80, kSoftwareReset = 0xe4;
constexpr uint8_t kPowerOn = 1u << 0, kAdcEnable = 1u << 1;

constexpr unsigned kSupplyDeadlineMS = 20000;  // from start to SwitchTouch's first LDO6 (it powers up in ~60 ms)
constexpr unsigned kErrorLimit = 20;           // samples in a row the sensor did not answer (about 2.3 s)
constexpr unsigned kUnclaimedLimit = 300;      // samples with no IOHIDEventDriver on the HID device (about 35 s)
#if DEVELOPMENT || DEBUG
constexpr unsigned kStatsSamples = 43;  // one log line per 5 s of samples
#endif

constexpr unsigned kSetTimeStamp = 17, kSetType = 19, kSetIntegerValue = 24;
constexpr uint32_t kAmbientLightType = 12, kFieldLevel = 0xc0000, kFieldChannel0 = 0xc0001, kFieldChannel1 = 0xc0002;

template <typename R, typename... A> R vcall(OSObject *object, unsigned slot, A... args)
{
    return reinterpret_cast<R (*)(OSObject *, A...)>((*reinterpret_cast<uintptr_t *const *>(object))[slot])(object,
                                                                                                            args...);
}

int writeRegister(uint8_t reg, uint8_t value)
{
    return tegra_i2c_write(kBus, kSensor, kCommand | reg, &value, 1);
}

uint64_t msSince(uint64_t then)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - then, &ns);
    return ns / 1000000;
}

// For the line that turns the sensor off: its supplies as they are now (L4T tegra210b01-fric-power-tree.dtsi
// bh1730fvc@29: als-vid = MAX77620 LDO6, als-vdd = vdd-3v3 = MAX77620 GPIO3 as an FPS output). LDO6_CFG 7:6 is the
// power mode (3 = on) and 5:0 the voltage (42 = 2.9 V), LDO6_CFG2 bit 3 its power-OK, FPS_LDO6 7:6 its sequencer
// source (3 = none); CNFG_GPIO3 bit 3 the output and bit 2 the input level (valid as an input only), AME_GPIO bit 3
// set when GPIO3 is the FPS output, FPS_GPIO3 its source and slots (hekate max77620.h, Linux max77620.h).
void describeSupplies(char *buffer, size_t size)
{
    constexpr unsigned kPmicBus = 5;
    constexpr uint8_t kPmic = 0x3c;
    constexpr uint8_t kLdo6Cfg = 0x2f;
    constexpr uint8_t kGpio3 = 0x39;
    constexpr uint8_t kAmeGpio = 0x40;
    constexpr uint8_t kFpsLdo6 = 0x4c;
    constexpr uint8_t kFpsGpio3 = 0x56;
    uint8_t ldo[2] = {};  // LDO6_CFG and LDO6_CFG2 (0x30): one read
    uint8_t gpio3 = 0;
    uint8_t ame = 0;
    uint8_t fps = 0;
    uint8_t fpsGpio3 = 0;
    int err = tegra_i2c_read(kPmicBus, kPmic, kLdo6Cfg, ldo, sizeof(ldo));
    if (!err)
        err = tegra_i2c_read(kPmicBus, kPmic, kGpio3, &gpio3, 1);
    if (!err)
        err = tegra_i2c_read(kPmicBus, kPmic, kAmeGpio, &ame, 1);
    if (!err)
        err = tegra_i2c_read(kPmicBus, kPmic, kFpsLdo6, &fps, 1);
    if (!err)
        err = tegra_i2c_read(kPmicBus, kPmic, kFpsGpio3, &fpsGpio3, 1);
    snprintf(buffer, size,
             "LDO6_CFG %#x (mode %u, %u mV) LDO6_CFG2 %#x (POK %u) FPS_LDO6 %#x, GPIO3 %#x AME %#x FPS_GPIO3 %#x (PMIC "
             "read %d)",
             ldo[0], ldo[0] >> 6, 800 + 50 * (ldo[0] & 0x3f), ldo[1], (static_cast<unsigned>(ldo[1]) >> 3) & 1u, fps,
             gpio3, ame, fpsGpio3, err);
}

#if DEVELOPMENT || DEBUG
// The bus itself, for the probe line after a sensor that never answered (only this driver uses I2C2, from this work
// loop, and TegraI2C has just recovered it from a NACK, so it is idle):
//   * SCL (PJ2) and SDA (PJ3) read as GPIO inputs (tegra_gpio_sample: released throughout; 1 = pulled up), with the
//     pins' CNF/OE/OUT/IN as found; next to them SFIO-mode GPIO_IN of the working, mostly idle I2C1 (PJ1, PJ0) and
//     I2C3 (PF0, PF1) lines, which shows whether that register means anything for a pin in SFIO mode.
//   * The PMC's I/O rails (pmc_t210.h, Linux tegra210_io_pads): NO_IOPOWER, PWR_DET_VAL (1 = 3.3 V; Horizon's boot
//     sets GPIO, AUDIO_HV, SPI_HV and SDMMC3 to 1.8 V, boot_change_voltage.cpp), IO_DPD_STATUS and IO_DPD2_STATUS.
//   * Who answers on I2C2: a one-byte read of every address 0x08-0x77.
void describeBus(char *buffer, size_t size)
{
    unsigned sclConfig = 0;
    unsigned sdaConfig = 0;
    const int scl = tegra_gpio_sample(TEGRA_GPIO(J, 2), &sclConfig);
    const int sda = tegra_gpio_sample(TEGRA_GPIO(J, 3), &sdaConfig);
    const int controls[4] = {tegra_gpio_read(TEGRA_GPIO(J, 1)), tegra_gpio_read(TEGRA_GPIO(J, 0)),
                             tegra_gpio_read(TEGRA_GPIO(F, 0)), tegra_gpio_read(TEGRA_GPIO(F, 1))};
    uint32_t pmc[4] = {};
    const uint32_t offsets[4] = {0x44, 0xe4, 0x1bc, 0x1c4};  // NO_IOPOWER, PWR_DET_VAL, IO_DPD(2)_STATUS
    for (unsigned i = 0; i < 4; ++i)
        pmc[i] = switch_pmc_read(offsets[i]);
    char acks[64] = "";
    size_t used = 0;
    unsigned found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        uint8_t byte = 0;
        if (tegra_i2c_transfer(kBus, addr, nullptr, 0, &byte, 1) != TEGRA_I2C_OK)
            continue;
        if (found++ < 10)
            used += static_cast<size_t>(snprintf(acks + used, sizeof(acks) - used, " 0x%02x", addr));
    }
    snprintf(buffer, size,
             "I2C2 as GPIO inputs SCL %d SDA %d (PJ2/PJ3 cnf|oe<<1|out<<2|in<<3 %#x %#x), SFIO GPIO_IN of the working "
             "I2C1 %d %d and I2C3 %d %d; PMC NO_IOPOWER %#x PWR_DET_VAL %#x IO_DPD_STATUS %#x IO_DPD2_STATUS %#x; "
             "%u addresses ACK on I2C2 0x08-0x77%s",
             scl, sda, sclConfig, sdaConfig, controls[0], controls[1], controls[2], controls[3], pmc[0], pmc[1], pmc[2],
             pmc[3], found, acks);
}
#endif
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class SwitchALS : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchALS) IOWorkLoop *loop = nullptr;
    IOTimerEventSource *clock = nullptr;        // every sampleUS while running
    IOTimerEventSource *supplyCheck = nullptr;  // kSupplyDeadlineMS for SwitchTouch's first LDO6
    IOService *hid = nullptr;
    unsigned gain = 2;  // 64x, as bh1730_init_config and hekate
    unsigned errors = 0, unclaimed = 0, skip = 0;
    bool lit = true, suspended = false, supply = false, identified = false, configured = false;
    bool supplied = false;  // LDO6 has come on at least once
    bool broken = false;    // a sensor or HID path that is not as expected: logged once, sampling off for this boot
    uint64_t supplyAt = 0;  // when LDO6 last came on (switch_als_supply)
    bool running = false;
    uint32_t sampleUS = kSampleUS;  // CoreBrightness's report interval, at least kSampleUS (reportIntervalSet)
    unsigned samples = 0, luxMin = ~0u, luxMax = 0;
    unsigned loggedLux = 0, loggedGain = ~0u;  // the last line's (noteStats); ~0u: none yet
    int loggedPanel = -1;

    static void ticked(OSObject *owner, IOTimerEventSource *sender);
    static void reportIntervalSet(void *context, const uint8_t *report, size_t length);
    static void supplyLate(OSObject *owner, IOTimerEventSource *sender);
    void update();
    void giveUp();
    int configure();
    void sample();
    void failed(int err);
    void dispatch(uint32_t level, uint32_t visible, uint32_t ir);
    void noteStats(uint32_t level, uint32_t visible, uint32_t ir);
    void setCondition(bool SwitchALS::*condition, bool value);

public:
    bool start(IOService *provider) override;
    void free() override;

    void setLit(bool on)
    {
        setCondition(&SwitchALS::lit, on);
    }

    void setSuspended(bool on)
    {
        setCondition(&SwitchALS::suspended, on);
    }

    void setSupply(bool on)
    {
        setCondition(&SwitchALS::supply, on);
    }

    bool published() const
    {
        return hid != nullptr;
    }
};

OSDefineMetaClassAndStructors(SwitchALS, IOService)

    static SwitchALS *volatile gALS;
static volatile bool gSupply;  // LDO6 as SwitchTouch last reported it, for a report that comes before start

// Work loop. The sensor samples while the panel is lit (or waking), the console awake and LDO6 on; otherwise it is
// powered down (it keeps its registers while LDO6 stays on) and its timer is off. Starting sets it up at once; the
// first tick is kFirstSampleUS later, a whole conversion after the set-up, because the sensor's registers still hold
// the last conversion from before it powered down.
// NOLINTNEXTLINE(misc-no-recursion): giveUp() sets broken first, so its update() only stops the sensor
void SwitchALS::update()
{
    const bool want = lit && !suspended && supply && !broken;
    if (want == __atomic_load_n(&running, __ATOMIC_RELAXED))
        return;
    if (want) {
        errors = 0;
        configured = false;  // the sensor may have lost power
        __atomic_store_n(&running, true, __ATOMIC_RELEASE);
        clock->setTimeoutUS(kFirstSampleUS);
        const int err = configure();
        if (err) {
            failed(err);  // the first tick tries again
        } else {
            configured = true;
        }
        return;
    }
    __atomic_store_n(&running, false, __ATOMIC_RELEASE);
    clock->cancelTimeout();
    if (configured)
        (void)writeRegister(RegControl, 0);  // power down; a sensor that is gone shows on the next start
    configured = false;
}

// Work loop. The ALS is not worth a boot: a part that is not a BH1730, a sensor that does not answer, a supply that
// never comes and an event path nothing takes each log one line (the caller's) and leave the sensor off; a lost
// display interrupt still panics.
// NOLINTNEXTLINE(misc-no-recursion): giveUp() sets broken first, so its update() only stops the sensor
void SwitchALS::giveUp()
{
    broken = true;
    update();
}

// First contact, once LDO6 has been on for at least 12 ms (switch_als_supply): I2C2 up (TegraI2C: pinmux, drive,
// clock, controller, bus clear; idempotent), the part identified and reset (the datasheet's special command, one byte;
// all registers to default, powered down), then gain, integration time and continuous conversion (bh1730_power_on and
// bh1730_init_config, hekate als_power_on). Later contacts (after LDO6 or SC7) only set it up again. The sensor is
// powered down until the last write (reset, LDO6 off, or update()'s power-down), so its first conversion is already
// this configuration's. Nothing touches the hardware before this.
int SwitchALS::configure()
{
    int err = 0;
    if (!identified) {
        uint8_t id = 0;
        err = tegra_i2c_init(kBus);
        if (err)
            return err;
        err = tegra_i2c_read(kBus, kSensor, kCommand | RegId, &id, 1);
        if (err)
            return err;
        // BH1730FVC PART_ID: part number 0111 in bits 7:4.
        if ((id >> 4) != 7) {
            IOLog("SwitchALS: off: I2C2 0x29 is not a BH1730 (id %#x)\n", id);
            broken = true;  // sample() turns the sensor off
            return -1;
        }
        err = tegra_i2c_write(kBus, kSensor, kSoftwareReset, nullptr, 0);
        if (err)
            return err;
        identified = true;
        IOLog("SwitchALS: started (BH1730 id %#x after %u failed tries, LDO6 on for %llu ms; a sample every %u us)\n",
              id, errors, msSince(supplyAt), kSampleUS);
    }
    err = writeRegister(RegGain, static_cast<uint8_t>(gain));
    if (!err)
        err = writeRegister(RegTiming, static_cast<uint8_t>(256 - kCycles));
    if (!err)
        err = writeRegister(RegControl, kPowerOn | kAdcEnable);
    skip = 0;
    return err;
}

void SwitchALS::dispatch(uint32_t level, uint32_t visible, uint32_t ir)
{
    OSObject *event = OSMetaClass::allocClassWithName("IOHIDEvent");
    if (!event) {
        IOLog("SwitchALS: off: OSMetaClass::allocClassWithName(\"IOHIDEvent\") failed (IOHIDFamily not loaded?)\n");
        giveUp();
        return;
    }
    vcall<void, uint32_t>(event, kSetType, kAmbientLightType);
    // initWithType allocates the event data; without it every accessor would dereference null.
    if (!*reinterpret_cast<void *const *>(reinterpret_cast<const uint8_t *>(event) + 0x10)) {
        IOLog("SwitchALS: off: IOHIDEvent setType(%u) left no event data (wrong vtable slot or no memory)\n",
              kAmbientLightType);
        event->release();
        giveUp();
        return;
    }
    vcall<void, uint64_t>(event, kSetTimeStamp, mach_absolute_time());
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSetIntegerValue, kFieldLevel, static_cast<int32_t>(level), 0);
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSetIntegerValue, kFieldChannel0, static_cast<int32_t>(visible),
                                                 0);
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSetIntegerValue, kFieldChannel1, static_cast<int32_t>(ir), 0);
    // kIOReturnNotReady until IOHIDEventDriver has started on the device; CoreBrightness never sees the sensor without it.
    const IOReturn status = switch_hid_dispatch_event(hid, event);
    event->release();
    if (status == kIOReturnSuccess) {
        unclaimed = 0;
    } else if (++unclaimed >= kUnclaimedLimit) {
        IOLog("SwitchALS: off: no IOHIDEventDriver took the ALS HID device in %u samples (dispatch %#x)\n",
              kUnclaimedLimit, status);
        giveUp();
    }
}

#if DEVELOPMENT || DEBUG
// To the log only: what the sensor saw over the last 5 s, and the panel level it saw it at, only when that changed
// since the last line (gain, panel level, or a reading more than an eighth, at least 2 lux, off the last line's
// lux), so a lit screen in steady light writes no line every 5 s.
void SwitchALS::noteStats(uint32_t level, uint32_t visible, uint32_t ir)
{
    if (level < luxMin)
        luxMin = level;
    if (level > luxMax)
        luxMax = level;
    if (++samples < kStatsSamples)
        return;
    const int panel = tegra_panel_brightness();
    const unsigned tolerance = loggedLux / 8 > 2 ? loggedLux / 8 : 2;
    if (gain != loggedGain || panel != loggedPanel || luxMin + tolerance < loggedLux ||
        luxMax > loggedLux + tolerance) {
        // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): nextGain() keeps gain under kGains
        os_log(OS_LOG_DEFAULT, "SwitchALS: %u samples, lux %u (%u..%u), ch0 %u ch1 %u gain %ux, panel %d", samples,
               level, luxMin, luxMax, visible, ir, kGainFactor[gain], panel);
        loggedLux = level;
        loggedGain = gain;
        loggedPanel = panel;
    }
    samples = 0;
    luxMin = ~0u;
    luxMax = 0;
}
#endif

// Work loop: a transfer to the sensor failed. kErrorLimit in a row turn it off with one line on the supplies. Debug
// kernels add one on the bus and the controller's last failure (TegraI2C) and, if it never answered, one on the lines,
// the PMC's rails and whoever answers on I2C2 (describeBus, a read of every address). The sampling is stopped first, so
// the probe runs on an idle bus.
// NOLINTNEXTLINE(misc-no-recursion): giveUp() sets broken first, so its update() only stops the sensor
void SwitchALS::failed(int err)
{
    if (broken) {
        giveUp();  // configure() logged why
        return;
    }
    if (++errors < kErrorLimit)
        return;
    giveUp();
    char line[400];
    describeSupplies(line, sizeof(line));
    IOLog("SwitchALS: off: BH1730 on I2C2 0x29 %s: I2C error %d (%s) %u times in a row, LDO6 on for %llu ms; %s\n",
          identified ? "stopped answering" : "never answered", err, err == TEGRA_I2C_ENACK ? "no ACK" : "bus fault",
          errors, msSince(supplyAt), line);
#if DEVELOPMENT || DEBUG
    tegra_i2c_describe(kBus, line, sizeof(line));
    IOLog("SwitchALS: %s\n", line);
    if (identified)
        return;
    describeBus(line, sizeof(line));
    IOLog("SwitchALS: probe: %s\n", line);
#endif
}

// Work loop, every kSampleUS while running. A sensor update() could not set up is set up here.
void SwitchALS::sample()
{
    uint8_t data[4] = {};  // DATA0 (visible) and DATA1 (infrared), little endian, in one auto-increment read
    const int err = configured ? tegra_i2c_read(kBus, kSensor, kCommand | RegData0, data, sizeof(data)) : configure();
    if (err) {
        failed(err);
        return;
    }
    errors = 0;
    if (!configured) {
        configured = true;
        return;
    }
    if (skip) {
        --skip;
        return;
    }
    const uint32_t visible = static_cast<uint32_t>(data[1]) << 8 | data[0];
    const uint32_t ir = static_cast<uint32_t>(data[3]) << 8 | data[2];
    if (!saturated(visible, ir) || gain == 0) {  // a clipped reading at a higher gain only steps the gain down
        const uint32_t level = lux(visible, ir, gain);
        dispatch(level, visible, ir);
#if DEVELOPMENT || DEBUG
        noteStats(level, visible, ir);
#endif
    }

    const unsigned next = nextGain(gain, visible, ir);
    if (next == gain)
        return;
    if (writeRegister(RegGain, static_cast<uint8_t>(next)))
        return;  // counted on the next read if the sensor is gone
    gain = next;
    skip = 1;  // the conversion in flight finishes at the old gain
}

void SwitchALS::ticked(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = static_cast<SwitchALS *>(owner);
    if (!__atomic_load_n(&self->running, __ATOMIC_ACQUIRE))
        return;
    sender->setTimeoutUS(__atomic_load_n(&self->sampleUS, __ATOMIC_RELAXED));
    self->sample();
}

// The Report Interval feature (report 1: a 32-bit microsecond count), on IOHIDEventDriver's thread: the next tick
// takes it. The sensor converts no faster than kSampleUS, so a shorter interval is held there, logged once.
void SwitchALS::reportIntervalSet(void *context, const uint8_t *report, size_t length)
{
    auto *self = static_cast<SwitchALS *>(context);
    if (length < 5)
        return;
    const uint32_t asked = static_cast<uint32_t>(report[1]) | static_cast<uint32_t>(report[2]) << 8 |
                           static_cast<uint32_t>(report[3]) << 16 | static_cast<uint32_t>(report[4]) << 24;
    if (asked && asked < kSampleUS) {
        switch_refusal("als", "interval-below-conversion",
                       "report interval %u us asked; the BH1730 converts every %u us, so it reports at that", asked,
                       kSampleUS);
    }
    __atomic_store_n(&self->sampleUS, asked > kSampleUS ? asked : kSampleUS, __ATOMIC_RELAXED);
}

// Work loop: SwitchTouch never turned LDO6 on (it did not start, or the panel never came up).
void SwitchALS::supplyLate(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchALS *>(owner);
    if (self->supplied || self->broken)
        return;
    IOLog("SwitchALS: off: LDO6, the sensor's I2C supply, did not come on in %u s (SwitchTouch turns it on with the "
          "touch panel)\n",
          kSupplyDeadlineMS / 1000);
    self->giveUp();
}

// Thread context: the panel goes dark or lights up (SwitchBacklight), the console sleeps or wakes (SwitchSuspend), or
// LDO6 comes or goes (SwitchTouch).
void SwitchALS::setCondition(bool SwitchALS::*condition, bool value)
{
    struct Change {
        bool SwitchALS::*condition;
        bool value;
    } change = {condition, value};

    loop->runAction(
        [](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchALS *>(owner);
            const auto *change = static_cast<const Change *>(arg);
            if (change->condition == &SwitchALS::supply && change->value && !self->supply) {
                self->supplyAt = mach_absolute_time();
                self->supplied = true;
                self->supplyCheck->cancelTimeout();
            }
            self->*change->condition = change->value;
            self->update();
            return kIOReturnSuccess;
        },
        this, &change);
}

bool SwitchALS::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    loop = IOWorkLoop::workLoop();
    clock = IOTimerEventSource::timerEventSource(this, &SwitchALS::ticked);
    supplyCheck = IOTimerEventSource::timerEventSource(this, &SwitchALS::supplyLate);
    if (!loop || !clock || !supplyCheck || loop->addEventSource(clock) != kIOReturnSuccess ||
        loop->addEventSource(supplyCheck) != kIOReturnSuccess) {
        panic("SwitchALS: work loop setup failed");
    }
    hid = switch_hid_create(kSwitchHIDLight);
    switch_hid_set_feature_handler(hid, &SwitchALS::reportIntervalSet, this);
    registerService();
    supplyCheck->setTimeoutMS(kSupplyDeadlineMS);
    setLit(switch_display_is_on());
    return true;
}

void SwitchALS::free()
{
    if (loop) {
        if (clock)
            loop->removeEventSource(clock);
        if (supplyCheck)
            loop->removeEventSource(supplyCheck);
    }
    OSSafeReleaseNULL(clock);
    OSSafeReleaseNULL(supplyCheck);
    OSSafeReleaseNULL(loop);
    IOService::free();
}

extern "C" int switch_als_start(void)
{
    if (gALS)
        return 0;
    IOService *resources = IOService::getResourceService();
    SwitchALS *candidate = OSTypeAlloc(SwitchALS);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources) ||
        !candidate->start(resources)) {
        panic("SwitchALS: attach or start failed");
    }
    __atomic_store_n(&gALS, candidate, __ATOMIC_SEQ_CST);
    // A report from SwitchTouch before gALS was set is in gSupply; one after it reaches setSupply itself (twice is
    // harmless).
    candidate->setSupply(__atomic_load_n(&gSupply, __ATOMIC_SEQ_CST));
    candidate->release();  // the attachment keeps it
    return 0;
}

extern "C" bool switch_als_published(void)
{
    const SwitchALS *als = __atomic_load_n(&gALS, __ATOMIC_ACQUIRE);
    return (als != nullptr) && als->published();
}

extern "C" void switch_als_set_active(bool active)
{
    if (SwitchALS *als = __atomic_load_n(&gALS, __ATOMIC_ACQUIRE))
        als->setLit(active);
}

extern "C" void switch_als_supply(bool on)
{
    __atomic_store_n(&gSupply, on, __ATOMIC_SEQ_CST);
    if (SwitchALS *als = __atomic_load_n(&gALS, __ATOMIC_SEQ_CST))
        als->setSupply(on);
}

extern "C" void switch_als_suspend(void)
{
    if (SwitchALS *als = __atomic_load_n(&gALS, __ATOMIC_ACQUIRE))
        als->setSuspended(true);
}

extern "C" void switch_als_resume(void)
{
    if (SwitchALS *als = __atomic_load_n(&gALS, __ATOMIC_ACQUIRE))
        als->setSuspended(false);
}
