// Host test of drivers/SwitchMotion/SwitchMotion.cpp: the real driver (included here, one translation unit) over
// switch_motion_stubs.h and register-level models of its IMUs behind tegra_spi_transfer. Each model keeps a
// register file with its datasheet reset values, applies reads' and writes' side effects (soft reset, read-to-clear
// status, latched INT2) and its timing rules, and records every SPI transfer, so each scenario checks the exact
// register traffic, the HID reports (decode, mount matrix, device frame, scale) and the log lines.
//   * ICM-42607-P / ICM-42607: TDK DS-000451 (the ICM-42670-P's map, which they share) reset values; a read or write
//     of an address the datasheet does not list, a write while it is still resetting (1 ms) or within 200 us of a sensor
//     leaving off, or a DEVICE_CONFIG without 4-wire SPI fails the test.
//     Its FIFO (r15-offload): 16-byte records (header, accelerometer, gyroscope, temperature, timestamp) for the sensors
//     FIFO_CONFIG5 (MREG1, through MADDR_W/M_W and MADDR_R/M_R, 10 us apart and only while MCLK_RDY) puts there, a
//     record count in FIFO_COUNTH/L (records or bytes, either endianness, as INTF_CONFIG0 says), FIFO_THS at every ODR
//     tick while the count is at or above the watermark (FIFO_WM_GT_TH), burst reads that stay on FIFO_DATA, a transfer
//     that ends inside a record fails the test (FIFO_RESUME_PARTIAL_RD 0 would read it again), stream mode drops the
//     oldest record when full and raises FIFO_FULL.
//   * ICM-40607 and LSM6DS3H: enough of their maps for identification, reset, set-up and one sample each (their reset
//     values as the driver's read-modify-writes expect them: inv_icm42600 and st_lsm6dsx).
#include <cinttypes>
#include <deque>
#include <array>
#include <gtest/gtest.h>
#include "bazel/testing/require.h"
#include "../SwitchMotion.cpp"
#include "orientation_pipeline_model.h"

// Every check is a GoogleTest failure at its call site.
#define check(ok, what) EXPECT_TRUE(ok) << (what)

static std::string hex(unsigned v)
{
    char text[8];
    std::snprintf(text, sizeof(text), "%02x", v);
    return text;
}

// --- IMU models -------------------------------------------------------------------------------------------------------
struct Chip {
    std::vector<std::string> ops;     // "R 75", "R 0b x12", "W 02 10"
    std::vector<std::string> errors;  // broken datasheet rules
    virtual ~Chip() = default;

    virtual void access()
    {}

    virtual uint8_t read(uint8_t reg) = 0;  // one byte of a read, with its side effects
    virtual void write(uint8_t reg, uint8_t value) = 0;

    virtual bool stays(uint8_t) const
    {
        return false;
    }  // a burst read that does not advance (FIFO_DATA)

    virtual void endTransfer()
    {}  // chip select released

    virtual bool line() const = 0;                                            // INT2, which drives PX2
    virtual void produce(const int16_t accel[3], const int16_t gyro[3]) = 0;  // the next output data rate tick

    void fail(const std::string &why)
    {
        errors.push_back(why);
    }
};

static Chip *gChip;

static const int16_t kNoData[3] = {-32768, -32768, -32768};  // what a sensor that is off reads

static void storeBigEndian(uint8_t *r, const int16_t v[3])
{
    for (unsigned i = 0; i < 3; ++i) {
        r[2 * i] = (uint8_t)((uint16_t)v[i] >> 8);
        r[2 * i + 1] = (uint8_t)v[i];
    }
}

struct Icm42607 : Chip {
    uint8_t r[0x80] = {};
    uint8_t mreg1[0x100] = {};
    const uint8_t who;
    bool resetBroken = false;  // the soft reset never reports RESET_DONE
    bool stuck = false;        // INT2 held high with no data-ready status
    bool mregBroken = false;   // M_W writes are not taken
    bool countBroken = false;  // FIFO_COUNTH reads 0xff
    uint64_t accessFrom = 0, writesFrom = 0, mregFrom = 0;
    uint8_t maddrW = 0, maddrR = 0;
    std::deque<std::array<uint8_t, 16>> fifo;
    unsigned fifoByte = 0;  // bytes of the front record read in this transfer
    uint16_t tick = 0;

    explicit Icm42607(uint8_t whoAmI = 0x60) : who(whoAmI)
    {
        resetValues();
    }

    void resetValues()
    {
        memset(r, 0, sizeof(r));
        const uint8_t reset[][2] = {{0x00, 0x08}, {0x01, 0x04}, {0x03, 0x2b}, {0x04, 0x0d}, {0x05, 0x05},
                                    {0x09, 0x80}, {0x20, 0x06}, {0x21, 0x06}, {0x23, 0x31}, {0x24, 0x41},
                                    {0x25, 0x08}, {0x26, 0x02}, {0x28, 0x01}, {0x2b, 0x10}, {0x34, 0x04},
                                    {0x35, 0x30}, {0x36, 0x4d}, {0x3a, 0x10}, {0x3f, 0xff}};
        for (const auto &v : reset)
            r[v[0]] = v[1];
        for (uint8_t a = 0x0b; a <= 0x16; a += 2)
            r[a] = 0x80;  // -32768: no data
        r[0x75] = who;
        memset(mreg1, 0, sizeof(mreg1));
        mreg1[0x01] = 0x20;  // FIFO_CONFIG5: FIFO_WM_GT_TH
        fifo.clear();
        fifoByte = 0;
    }

    unsigned watermark() const
    {
        return r[0x29] | (r[0x2a] & 0x0f) << 8;
    }

    unsigned countValue() const
    {
        return (r[0x35] & 0x40) ? (unsigned)fifo.size() : (unsigned)fifo.size() * 16;
    }

    bool stays(uint8_t reg) const override
    {
        return reg == 0x3f;
    }

    void endTransfer() override
    {
        if (fifoByte)
            fail("a transfer ended inside a FIFO record");
        fifoByte = 0;
    }

    static bool writable(uint8_t reg)
    {
        return (reg >= 0x01 && reg <= 0x06) || (reg >= 0x1f && reg <= 0x2e) || reg == 0x35 || reg == 0x36 ||
               (reg >= 0x79 && reg <= 0x7e);
    }

    static bool listed(uint8_t reg)
    {
        return writable(reg) || reg == 0x00 || (reg >= 0x09 && reg <= 0x18) || reg == 0x1d || reg == 0x1e ||
               (reg >= 0x2f && reg <= 0x34) || (reg >= 0x39 && reg <= 0x3f) || reg == 0x75;
    }

    void access() override
    {
        if (gNow < accessFrom)
            fail("access within 1 ms of the soft reset");
        if (gNow < mregFrom)
            fail("access within 10 us of an MREG access");
    }

    uint8_t read(uint8_t reg) override
    {
        if (reg >= 0x80 || !listed(reg)) {
            fail("read of unlisted register " + hex(reg));
            return 0;
        }
        if (reg == 0x3d || reg == 0x3e) {
            const unsigned count = countValue();
            const bool big = r[0x35] & 0x20;
            if (countBroken)
                return 0xff;
            return (uint8_t)((reg == 0x3d) == big ? count >> 8 : count);
        }
        if (reg == 0x3f) {
            if (fifo.empty())
                return 0x80;
            const uint8_t value = fifo.front()[fifoByte++];
            if (fifoByte == 16)
                fifo.pop_front(), fifoByte = 0;
            return value;
        }
        if (reg == 0x7e) {
            if (!(r[0x00] & 0x08))
                fail("M_R read without MCLK");
            mregFrom = gNow + 10000;
            return mreg1[maddrR];
        }
        const uint8_t value = r[reg];
        if (reg >= 0x39 && reg <= 0x3c)
            r[reg] = 0;  // INT_STATUS_DRDY .. INT_STATUS3: cleared by the read
        return value;
    }

    void write(uint8_t reg, uint8_t value) override
    {
        if (reg >= 0x80 || !writable(reg))
            return fail("write of " + hex(value) + " to read-only/unlisted " + hex(reg));
        if (gNow < writesFrom)
            fail("write to " + hex(reg) + " within 200 us of a sensor leaving off");
        switch (reg) {
        case 0x01:
            if ((value & 0x05) != 0x04)
                fail("DEVICE_CONFIG " + hex(value) + ": not 4-wire SPI mode 0/3");
            r[reg] = value & 0x05;
            return;
        case 0x02:
            if (value & 0x10) {
                resetValues();
                if (resetBroken)
                    r[0x3a] = 0;
                accessFrom = gNow + 1000000;
            }
            return;
        case 0x7a:
            maddrW = value;
            return;
        case 0x7d:
            maddrR = value;
            mregFrom = gNow + 10000;
            return;
        case 0x7b:
            if (!(r[0x00] & 0x08))
                fail("M_W write without MCLK");
            if (r[0x79])
                fail("M_W with BLK_SEL_W not MREG1");
            if (!mregBroken)
                mreg1[maddrW] = value;
            mregFrom = gNow + 10000;
            return;
        case 0x28:
            if (value & 1)
                fifo.clear(), fifoByte = 0;  // bypassed: empty
            r[reg] = value;
            return;
        case 0x29:
        case 0x2a:
            if (!fifo.empty())
                fail("FIFO_WM written with records in the FIFO");
            r[reg] = value;
            return;
        case 0x1f: {
            const uint8_t old = r[0x1f];
            if ((value & 0x0c) == 0x08 || (value & 0x60))
                fail("PWR_MGMT0 " + hex(value) + ": reserved bits/mode");
            if (((old & 0x0c) == 0 && (value & 0x0c)) || ((old & 0x03) < 2 && (value & 0x03) >= 2)) {
                writesFrom = gNow + 200000;
            }
            if ((value & 0x03) < 2)
                storeBigEndian(r + 0x0b, kNoData);
            if ((value & 0x0c) != 0x0c)
                storeBigEndian(r + 0x11, kNoData);
            r[reg] = value;
            return;
        }
        default:
            r[reg] = value;
        }
    }

    bool line() const override
    {
        const bool latched = (r[0x06] & 0x38) == 0x38;
        return stuck || ((r[0x2d] & 0x08) && (r[0x39] & 1) && latched) ||
               ((r[0x2d] & 0x04) && (r[0x3a] & 0x04) && latched);
    }

    void produce(const int16_t accel[3], const int16_t gyro[3]) override
    {
        const bool accelOn = (r[0x1f] & 0x03) == 0x03, gyroOn = (r[0x1f] & 0x0c) == 0x0c;
        if (!accelOn && !gyroOn)
            return fail("a sample with both sensors off");
        for (uint8_t config : {(uint8_t)0x20, (uint8_t)0x21}) {
            const unsigned code = r[config] & 0x0f;
            if (code < 5 || code > 12)
                fail("ODR code " + hex(code) + " in " + hex(config) + " (low-noise: 5..12)");
        }
        if (accelOn)
            storeBigEndian(r + 0x0b, accel);
        if (gyroOn)
            storeBigEndian(r + 0x11, gyro);
        r[0x39] |= 1;
        const bool fifoAccel = accelOn && (mreg1[0x01] & 0x01), fifoGyro = gyroOn && (mreg1[0x01] & 0x02);
        if ((r[0x28] & 1) || (!fifoAccel && !fifoGyro))
            return;
        std::array<uint8_t, 16> record = {};
        record[0] = (uint8_t)((fifoAccel ? 0x40 : 0) | (fifoGyro ? 0x20 : 0) | 0x08);
        storeBigEndian(record.data() + 1, fifoAccel ? accel : kNoData);
        storeBigEndian(record.data() + 7, fifoGyro ? gyro : kNoData);
        record[14] = (uint8_t)(tick >> 8), record[15] = (uint8_t)tick;
        tick = (uint16_t)(tick + 5000 / 16);
        if (fifo.size() == 144)
            fifo.pop_front(), r[0x3a] |= 0x02;  // stream mode: the oldest goes, FIFO_FULL
        fifo.push_back(record);
        if (watermark() && countValue() >= watermark() && (mreg1[0x01] & 0x20))
            r[0x3a] |= 0x04;  // FIFO_THS
    }
};

struct Icm40607 : Chip {
    uint8_t r[0x80] = {};

    Icm40607()
    {
        resetValues();
    }

    void resetValues()
    {
        memset(r, 0, sizeof(r));
        const uint8_t reset[][2] = {{0x13, 0x05}, {0x2d, 0x10}, {0x4c, 0x30}, {0x4f, 0x06},
                                    {0x50, 0x06}, {0x64, 0x10}, {0x65, 0x10}, {0x75, 0x38}};
        for (const auto &v : reset)
            r[v[0]] = v[1];
        for (uint8_t a = 0x1f; a <= 0x2a; a += 2)
            r[a] = 0x80;
    }

    uint8_t read(uint8_t reg) override
    {
        const uint8_t value = r[reg & 0x7f];
        if (reg == 0x2d)
            r[reg] = 0;  // INT_STATUS: cleared by the read
        return value;
    }

    void write(uint8_t reg, uint8_t value) override
    {
        const bool writable = reg == 0x11 || reg == 0x13 || reg == 0x14 || reg == 0x16 ||
                              (reg >= 0x4c && reg <= 0x50) || (reg >= 0x63 && reg <= 0x68) || reg == 0x76;
        if (!writable)
            return fail("write of " + hex(value) + " to read-only/unlisted " + hex(reg));
        if (reg == 0x11 && (value & 1))
            return resetValues();
        r[reg] = value;
    }

    bool line() const override
    {
        return (r[0x68] & 0x08) && (r[0x2d] & 0x08) && (r[0x14] & 0x38) == 0x38 && !(r[0x64] & 0x10);
    }

    void produce(const int16_t accel[3], const int16_t gyro[3]) override
    {
        if ((r[0x4e] & 0x03) == 0x03)
            storeBigEndian(r + 0x1f, accel);
        if ((r[0x4e] & 0x0c) == 0x0c)
            storeBigEndian(r + 0x25, gyro);
        r[0x2d] |= 0x08;
    }
};

struct Lsm6ds3h : Chip {
    uint8_t r[0x80] = {};

    Lsm6ds3h()
    {
        resetValues();
    }

    void resetValues()
    {
        memset(r, 0, sizeof(r));
        r[0x0f] = 0x69;
        r[0x12] = 0x04;  // CTRL3_C: IF_INC
    }

    uint8_t read(uint8_t reg) override
    {
        const uint8_t value = r[reg & 0x7f];
        if (reg == 0x27)
            r[0x1e] &= ~2;  // OUTZ_H_G read: GDA clears
        if (reg == 0x2d)
            r[0x1e] &= ~1;  // OUTZ_H_XL read: XLDA clears
        return value;
    }

    void write(uint8_t reg, uint8_t value) override
    {
        if (reg < 0x01 || reg > 0x19 || reg == 0x0f)
            return fail("write to read-only/unlisted " + hex(reg));
        if (reg == 0x12 && (value & 1))
            return resetValues();
        r[reg] = value;
    }

    bool line() const override
    {
        return r[0x0e] & r[0x1e] & 0x03;
    }

    void produce(const int16_t accel[3], const int16_t gyro[3]) override
    {
        for (unsigned i = 0; i < 3; ++i) {
            if (r[0x11] >> 4)
                r[0x22 + 2 * i] = (uint8_t)gyro[i], r[0x23 + 2 * i] = (uint8_t)((uint16_t)gyro[i] >> 8);
            if (r[0x10] >> 4)
                r[0x28 + 2 * i] = (uint8_t)accel[i], r[0x29 + 2 * i] = (uint8_t)((uint16_t)accel[i] >> 8);
        }
        r[0x1e] |= (r[0x10] >> 4 ? 1 : 0) | (r[0x11] >> 4 ? 2 : 0);
    }
};

struct UnknownPart : Chip {
    uint8_t read(uint8_t reg) override
    {
        return reg == 0x75 ? 0x12 : reg == 0x0f ? 0x34 : 0;
    }

    void write(uint8_t reg, uint8_t) override
    {
        fail("write to an unidentified part: " + hex(reg));
    }

    bool line() const override
    {
        return false;
    }

    void produce(const int16_t *, const int16_t *) override
    {}
};

// --- Kernel interfaces the driver calls -------------------------------------------------------------------------------
static unsigned gSpiSuspends = 0, gSpiResumes = 0;
// Set by a scenario: every transfer from then on fails with it (the bus or the part broken), counted in gSpiFailed.
static int gSpiFailure = 0;
static unsigned gSpiFailed = 0;

extern "C" int tegra_spi_init(unsigned bus)
{
    return bus == 4 ? 0 : -1;
}

extern "C" void tegra_spi_suspend(unsigned bus)
{
    gSpiSuspends += bus == 4;
}

extern "C" void tegra_spi_resume(unsigned bus)
{
    gSpiResumes += bus == 4;
}

extern "C" int tegra_spi_transfer(unsigned bus, unsigned cs, const void *txv, void *rxv, size_t len)
{
    REQUIRE(bus == 4 && cs == 0 && txv && len >= 2 && len <= TEGRA_SPI_MAX_TRANSFER);
    const auto *tx = static_cast<const uint8_t *>(txv);
    auto *rx = static_cast<uint8_t *>(rxv);
    const uint8_t reg = tx[0] & 0x7f;
    if (gSpiFailure) {
        ++gSpiFailed;
        return gSpiFailure;
    }
    gChip->access();
    if (tx[0] & 0x80) {
        REQUIRE(rx);
        gChip->ops.push_back("R " + hex(reg) + (len > 2 ? " x" + std::to_string(len - 1) : ""));
        rx[0] = 0xff;
        for (size_t i = 1; i < len; ++i)
            rx[i] = gChip->read(gChip->stays(reg) ? reg : (uint8_t)(reg + i - 1));
    } else {
        if (len != 2)
            gChip->fail("multi-byte write at " + hex(reg));
        gChip->ops.push_back("W " + hex(reg) + " " + hex(tx[1]));
        gChip->write(reg, tx[1]);
    }
    gChip->endTransfer();
    return 0;
}

static IOService gNub;  // PX2's interrupt nub

extern "C" IOService *tegra_gpio_interrupt_nub(unsigned gpio, unsigned trigger)
{
    REQUIRE(gpio == TEGRA_GPIO_IMU_INT && trigger == TEGRA_GPIO_LEVEL_HIGH);
    return &gNub;
}

extern "C" void tegra_gpio_describe(unsigned gpio, char *buffer, size_t size)
{
    std::snprintf(buffer, size, "GPIO %u (stub)", gpio);
}

extern "C" void tegra_gpio_check_delivery(unsigned gpio, const char *who)
{
    if (gChip->line())
        panic("%s: GPIO %u interrupt lost (stub)", who, gpio);
}

// ArmGIC's leeway timer (r16-idle): the harness only sees the interval.
int switch_check_after_ms(IOTimerEventSource *timer, uint32_t ms)
{
    return timer->setTimeoutMS(ms);
}

// SwitchBacklight's panel state (switch_display_is_on): off unless a scenario lights it, so the orientation detector
// stays out of the scenarios written before it ran (r17).
static bool gDisplayLit = false;

extern "C" bool switch_display_is_on(void)
{
    return gDisplayLit;
}

struct HidDevice : IOService {
    SwitchHIDKind kind = kSwitchHIDTouch;
    SwitchHIDFeatureHandler handler = nullptr;
    void *context = nullptr;
    std::vector<SwitchMotionReport> reports;
    std::vector<uint64_t> times;  // switch_hid_post_at's sample times (0: switch_hid_post)
    // The device orientation service: the reports IOHIDFamily took, their report times (IOHIDDevice::handleReport's
    // clock_get_uptime, or switch_hid_post_at's time) and their bit masks; `refuse` reports are refused first, as
    // IOHIDDevice refuses them until it is ready for input reports.
    std::vector<SwitchOrientationReport> orientationReports;
    std::vector<uint64_t> orientationTimes;
    std::vector<uint8_t> orientations;
    unsigned refuse = 0, refused = 0;
};

static bool gTimedPosts = true;  // switch_hid_supports_timestamps
static std::vector<HidDevice *> gHid;

IOService *switch_hid_create(SwitchHIDKind kind)
{
    auto *device = new HidDevice;
    device->kind = kind;
    gHid.push_back(device);
    return device;
}

constexpr IOReturn kNotReady = (IOReturn)0xe00002d7;  // IOHIDDevice::handleReport before it is ready

static IOReturn postOrientation(HidDevice *device, const void *report, size_t length, uint64_t time)
{
    REQUIRE(length == sizeof(SwitchOrientationReport));
    SwitchOrientationReport copy;
    memcpy(&copy, report, sizeof(copy));
    REQUIRE(copy.id == 1);
    REQUIRE((copy.orientation[0] == 0) != (copy.orientation[1] == 0));
    if (device->refuse) {
        --device->refuse;
        ++device->refused;
        return kNotReady;
    }
    device->orientationReports.push_back(copy);
    device->orientationTimes.push_back(time);
    device->orientations.push_back((uint8_t)(copy.orientation[0] | copy.orientation[1]));
    return kIOReturnSuccess;
}

IOReturn switch_hid_post(IOService *device, const void *report, size_t length)
{
    if (static_cast<HidDevice *>(device)->kind == kSwitchHIDOrientation)
        return postOrientation(static_cast<HidDevice *>(device), report, length, mach_absolute_time());
    REQUIRE(length == sizeof(SwitchMotionReport));
    SwitchMotionReport copy;
    memcpy(&copy, report, sizeof(copy));
    static_cast<HidDevice *>(device)->reports.push_back(copy);
    static_cast<HidDevice *>(device)->times.push_back(0);
    return kIOReturnSuccess;
}

IOReturn switch_hid_post_at(IOService *device, const void *report, size_t length, uint64_t timestamp)
{
    REQUIRE(gTimedPosts);
    if (static_cast<HidDevice *>(device)->kind == kSwitchHIDOrientation)
        return postOrientation(static_cast<HidDevice *>(device), report, length, timestamp);
    REQUIRE(length == sizeof(SwitchMotionReport));
    SwitchMotionReport copy;
    memcpy(&copy, report, sizeof(copy));
    static_cast<HidDevice *>(device)->reports.push_back(copy);
    static_cast<HidDevice *>(device)->times.push_back(timestamp);
    return kIOReturnSuccess;
}

bool switch_hid_supports_timestamps(IOService *)
{
    return gTimedPosts;
}

void switch_hid_set_feature_handler(IOService *device, SwitchHIDFeatureHandler handler, void *context)
{
    static_cast<HidDevice *>(device)->handler = handler;
    static_cast<HidDevice *>(device)->context = context;
}

// --- Scenario helpers ---------------------------------------------------------------------------------------------------
static HidDevice *hid(SwitchHIDKind kind)
{
    for (HidDevice *device : gHid)
        if (device->kind == kind)
            return device;
    return nullptr;
}

// A client's ReportInterval and BatchInterval (microseconds) as IOHIDEventDriver sends them: report ID 1, then the
// Report Interval and Report Latency fields, 32 bits each (SwitchHIDDevice.cpp MOTION_DESCRIPTOR).
static void setFeature(SwitchHIDKind kind, uint32_t us, uint32_t batch)
{
    HidDevice *device = hid(kind);
    REQUIRE(device && device->handler);
    const uint8_t report[9] = {1,
                               (uint8_t)us,
                               (uint8_t)(us >> 8),
                               (uint8_t)(us >> 16),
                               (uint8_t)(us >> 24),
                               (uint8_t)batch,
                               (uint8_t)(batch >> 8),
                               (uint8_t)(batch >> 16),
                               (uint8_t)(batch >> 24)};
    device->handler(device->context, report, sizeof(report));
}

static void setInterval(SwitchHIDKind kind, uint32_t us)
{
    setFeature(kind, us, 0);
}

static IOInterruptEventSource *dataReady()
{
    for (IOInterruptEventSource *source : IOInterruptEventSource::all)
        if (source->provider == &gNub)
            return source;
    return nullptr;
}

// PX2 is level-triggered: while INT2 is high and the source enabled, its action runs again (at most `limit` times).
static unsigned deliver(unsigned limit = 4)
{
    unsigned fired = 0;
    for (IOInterruptEventSource *source = dataReady(); source && source->enabled && gChip->line() && fired < limit;) {
        source->interruptOccurred(nullptr, &gNub, 0);
        ++fired;
    }
    return fired;
}

static void expectOps(const std::vector<std::string> &want, const std::string &what)
{
    const bool same = gChip->ops == want;
    check(same, what + ": register traffic");
    if (!same) {
        std::fprintf(stderr, "  want:");
        for (const std::string &op : want)
            std::fprintf(stderr, " [%s]", op.c_str());
        std::fprintf(stderr, "\n  got: ");
        for (const std::string &op : gChip->ops)
            std::fprintf(stderr, " [%s]", op.c_str());
        std::fprintf(stderr, "\n");
    }
    gChip->ops.clear();
}

static bool logged(const std::string &line)
{
    for (const std::string &l : gLog)
        if (l == line)
            return true;
    return false;
}

static void expectLog(const std::string &line, const std::string &what)
{
    check(logged(line), what + ": log line \"" + line + "\"");
    if (!logged(line))
        for (const std::string &l : gLog)
            std::fprintf(stderr, "  log: %s\n", l.c_str());
}

static bool sameReport(const SwitchMotionReport &r, int32_t x, int32_t y, int32_t z)
{
    return r.id == 1 && r.x == x && r.y == y && r.z == z;
}

static void expectReports(SwitchHIDKind kind, size_t count, int32_t x, int32_t y, int32_t z, const std::string &what)
{
    const std::vector<SwitchMotionReport> &reports = hid(kind)->reports;
    bool ok = reports.size() == count;
    for (const SwitchMotionReport &r : reports)
        ok = ok && sameReport(r, x, y, z);
    check(ok, what + ": " + std::to_string(count) + " reports of (" + std::to_string(x) + ", " + std::to_string(y) +
                  ", " + std::to_string(z) + ")");
    if (!ok)
        for (const SwitchMotionReport &r : reports)
            std::fprintf(stderr, "  report: %d %d %d\n", r.x, r.y, r.z);
}

static void expectClean(const std::string &what)
{
    check(gChip->errors.empty(), what + ": datasheet rules kept");
    for (const std::string &e : gChip->errors)
        std::fprintf(stderr, "  %s: %s\n", what.c_str(), e.c_str());
}

static bool start(Chip &chip)
{
    gChip = &chip;
    gLog.clear();
    return switch_motion_start() == 0;
}

static void teardown()
{
    if (gMotion)
        gMotion->detach(IOService::getResourceService());  // the attachment's reference: frees it
    gMotion = nullptr;
    IOInterruptEventSource::all.clear();
    IOTimerEventSource::last = nullptr;
    for (HidDevice *device : gHid)
        device->release();
    gHid.clear();
    gLog.clear();
    gChip = nullptr;
    gTimedPosts = true;
    gDisplayLit = false;
    gSpiFailure = 0;
    gSpiFailed = 0;
}

// What start() sends an ICM-42607 after identifying it: reset (TDK inv_imu_device_reset), interface and INT2 set-up,
// then both sensors off at the lowest rate (12.5 Hz: ODR 12, 16 Hz filter).
static const std::vector<std::string> k42607Start = {
    "R 75",    "R 0f",                                      // identify, nothing written first
    "W 7c 00", "W 79 00", "W 02 10", "W 01 04", "R 3a",     // reset, 4-wire restated, RESET_DONE
    "R 36",    "W 36 41", "W 05 05", "R 35",    "W 35 73",  // I3C off + PLL, SPI slew, I2C off, big-endian, records
    "W 06 38", "W 2b 00",                                   // INT2 latched push-pull high; INT1 nothing
    "W 2d 00", "W 20 0c", "W 21 2c", "W 23 37", "W 24 47", "W 1f 00",  // off
};

// --- Scenarios ------------------------------------------------------------------------------------------------------
static void decodeAndMapping()
{
    // ICM: big-endian accelerometer then gyroscope; -32768 marks a sensor that is off.
    const uint8_t icm[12] = {0x00, 0x64, 0xff, 0x38, 0x10, 0x00, 0x00, 0xa4, 0xfe, 0xb8, 0x00, 0x10};
    int16_t accel[3], gyro[3];
    bool fresh[2];
    decodeICM(icm, true, accel, gyro, fresh);
    check(accel[0] == 100 && accel[1] == -200 && accel[2] == 4096 && gyro[0] == 164 && gyro[1] == -328 &&
              gyro[2] == 16 && fresh[Accel] && fresh[Gyro],
          "decodeICM: big-endian words");
    decodeICM(icm, false, accel, gyro, fresh);
    check(!fresh[Accel] && !fresh[Gyro], "decodeICM: no data-ready, nothing fresh");
    uint8_t off[12];
    memcpy(off, icm, sizeof(off));
    off[6] = 0x80, off[7] = 0x00;
    decodeICM(off, true, accel, gyro, fresh);
    check(fresh[Accel] && !fresh[Gyro], "decodeICM: a gyroscope axis at -32768 is not fresh");

    // ST: STATUS, then gyroscope and accelerometer little-endian from byte 4.
    const uint8_t stb[16] = {0x01, 0, 0, 0, 0xa4, 0x00, 0xb8, 0xfe, 0x10, 0x00, 0x64, 0x00, 0x38, 0xff, 0xfe, 0xef};
    decodeST(stb, accel, gyro, fresh);
    check(accel[0] == 100 && accel[1] == -200 && accel[2] == -4098 && gyro[0] == 164 && gyro[1] == -328 &&
              gyro[2] == 16 && fresh[Accel] && !fresh[Gyro],
          "decodeST: little-endian words, STATUS bits");

    // Device frame (portrait panel: +X = console bottom, +Y = console right, +Z out of the screen), gravity vector.
    // ICM (X = -y, Y = +x, Z = +z): chip +z out of the screen, so lying face up (+1 g on z) reads z = -1 g.
    const int16_t up[3] = {0, 0, 4096}, chipX[3] = {4096, 0, 0}, chipY[3] = {0, 4096, 0}, a[3] = {100, -200, 4096};
    const auto r = [](const FamilyInfo &f, Sensor s, const int16_t *v) {
        return deviceReport(f, s, v);
    };
    check(sameReport(r(kICM42607, Accel, up), 0, 0, -1000000), "ICM-42607 face up: z = -1 g");
    check(sameReport(r(kICM42607, Accel, chipX), 1000000, 0, 0), "ICM-42607 chip +x up: device x = +1 g (bottom)");
    check(sameReport(r(kICM42607, Accel, chipY), 0, 1000000, 0), "ICM-42607 chip +y up: device y = +1 g (right)");
    check(sameReport(r(kICM42607, Accel, a), 24414, -48828, -1000000), "ICM-42607 accel scale 1e6/4096 ug");
    const int16_t g[3] = {164, -328, 16};
    check(sameReport(r(kICM42607, Gyro, g), -174532, 349065, 17027), "ICM-42607 gyro: rad/s, rotated not negated");
    check(sameReport(r(kICM40607, Accel, a), 24414, -48828, -1000000), "ICM-40607 shares the ICM mount");
    // ST (X = +y, Y = +x, Z = -z): chip -z out of the screen.
    const int16_t s[3] = {100, -200, -4098};
    check(sameReport(r(kST, Accel, s), 24400, 48800, -999912), "ST accel: mount and 0.244 mg/LSB");

    // Rates: the ICM-42607's ODR codes are one above the ICM-40607's.
    const uint8_t codes42607[5] = {12, 11, 10, 9, 8}, codes40607[5] = {11, 10, 9, 8, 7};
    bool rates = true;
    for (unsigned i = 0; i < 5; ++i) {
        rates = rates && kICM42607.rates[i].code == codes42607[i] && kICM40607.rates[i].code == codes40607[i] &&
                kICM42607.rates[i].milliHz == kICM40607.rates[i].milliHz;
    }
    check(rates, "ODR codes: ICM-42607 12.5 Hz = 12 .. 200 Hz = 8; ICM-40607 11 .. 7");

    // FIFO records: header, then the register burst's layout; the empty marker and 20-byte records stop a drain.
    uint8_t record[16] = {0x68};
    memcpy(record + 1, icm, sizeof(icm));
    check(decodeFifoRecord(record, accel, gyro, fresh) && accel[0] == 100 && accel[2] == 4096 && gyro[1] == -328 &&
              fresh[Accel] && fresh[Gyro],
          "decodeFifoRecord: accelerometer and gyroscope after the header");
    record[0] = 0x28;
    check(decodeFifoRecord(record, accel, gyro, fresh) && !fresh[Accel] && fresh[Gyro],
          "decodeFifoRecord: a record without the accelerometer bit");
    record[0] = 0x80;
    check(!decodeFifoRecord(record, accel, gyro, fresh), "decodeFifoRecord: the empty marker");
    record[0] = 0x78;
    check(!decodeFifoRecord(record, accel, gyro, fresh), "decodeFifoRecord: a 20-byte record");
    // Watermarks: whole sample periods inside the batch interval, 1 for none, capped.
    check(watermarkFor(15000, 5000) == 3 && watermarkFor(20000, 20000) == 1 && watermarkFor(0, 5000) == 1 &&
              watermarkFor(15000, 10000) == 1 && watermarkFor(1000000, 5000) == 24 &&
              watermarkFor(1000000, 20000) == 12,
          "watermarkFor: 15 ms at 200 Hz = 3; caps at 24 records and 250 ms");
}

// The r7p8 console: an ICM-42607-P straight from power-on (0x0f = 0x80, 0x75 = 0x60), through a whole client session.
static void icm42607Session()
{
    Icm42607 chip;
    check(start(chip), "42607: started");
    check(IOService::getResourceService() && gMotion && gMotion->imu == "ICM-42607-P", "42607: IMU property");
    expectOps(k42607Start, "42607 start");
    expectLog("IOLog: SwitchMotion: started (ICM-42607-P on SPI4, data-ready on PX2)", "42607 start");
    check(dataReady() && !dataReady()->enabled, "42607: data-ready masked while idle");

    // UIAccelerometer-style client at 100 Hz: ODR code 9 (not the ICM-40607's 8), 25 Hz filter, accelerometer LN.
    setInterval(kSwitchHIDAccelerometer, 10000);
    expectOps({"W 20 09", "W 21 29", "W 23 36", "W 24 46", "W 1f 03", "W 2d 08"}, "42607 accel 100 Hz");
    expectLog("os_log: SwitchMotion: 100000 mHz, accel 1 (10000 us), gyro 0 (0 us), batch 0 us, FIFO watermark 1",
              "42607 accel 100 Hz");
    check(dataReady()->enabled && IOTimerEventSource::last->armed && IOTimerEventSource::last->ms == 1000,
          "42607: data-ready and the 1 s watchdog armed");

    const int16_t accel[3] = {100, -200, 4096}, gyro[3] = {164, -328, 16}, none[3] = {0, 0, 0};
    gNow += 10000000;  // 10 ms: inside the accelerometer's 20 ms settle
    chip.produce(accel, none);
    check(deliver() == 1, "42607: one interrupt per sample (the status read drops INT2)");
    expectOps({"R 39", "R 0b x12"}, "42607 sample: status, then data");
    check(hid(kSwitchHIDAccelerometer)->reports.empty(), "42607: samples inside the settle time dropped");
    gNow += 15000000;
    chip.produce(accel, none);
    check(deliver() == 1, "42607: second sample");
    chip.produce(accel, none);
    check(deliver() == 1, "42607: third sample");
    gChip->ops.clear();
    expectReports(kSwitchHIDAccelerometer, 2, 24414, -48828, -1000000, "42607 accel reports");
    expectLog(
        "IOLog: SwitchMotion: ICM-42607-P first accelerometer sample: raw 100 -200 4096, device frame 24 -48 -1000 "
        "mg (face up: z near -1000; upright: x near +1000)",
        "42607 first sample");
    unsigned firstLines = 0;
    for (const std::string &l : gLog)
        firstLines += l.find("first accelerometer sample") != std::string::npos;
    check(firstLines == 1, "42607: the first sample is logged once");
    IOTimerEventSource::last->fire();
    check(IOTimerEventSource::last->armed, "42607: watchdog re-armed after samples");

    // CoreMotion's gyroscope at 200 Hz joins: both at ODR 8 with the 53 Hz filter; the accelerometer is decimated 2:1.
    setInterval(kSwitchHIDGyro, 5000);
    expectOps({"W 20 08", "W 21 28", "W 23 34", "W 24 44", "W 1f 0f", "W 2d 08"}, "42607 gyro 200 Hz");
    expectLog("os_log: SwitchMotion: 200000 mHz, accel 1 (10000 us), gyro 1 (5000 us), batch 0 us, FIFO watermark 1",
              "42607 gyro 200 Hz");
    gNow += 30000000;  // inside the gyroscope's 60 ms
    chip.produce(accel, gyro);
    deliver();
    check(hid(kSwitchHIDGyro)->reports.empty(), "42607: gyroscope samples inside its 60 ms settle dropped");
    gNow += 31000000;
    hid(kSwitchHIDAccelerometer)->reports.clear();
    for (unsigned i = 0; i < 4; ++i) {
        chip.produce(accel, gyro);
        deliver();
    }
    gChip->ops.clear();
    expectReports(kSwitchHIDGyro, 4, -174532, 349065, 17027, "42607 gyro reports");
    expectReports(kSwitchHIDAccelerometer, 2, 24414, -48828, -1000000, "42607 accel decimated to 100 Hz");

    // Both clients gone: routing off first, then the sensors (at the lowest rate's settings, as at start);
    // data-ready masked, watchdog cancelled.
    setInterval(kSwitchHIDGyro, 0);
    expectOps({"W 20 09", "W 21 29", "W 23 36", "W 24 46", "W 1f 03", "W 2d 08"}, "42607 gyro gone");
    setInterval(kSwitchHIDAccelerometer, 0);
    expectOps({"W 2d 00", "W 20 0c", "W 21 2c", "W 23 37", "W 24 47", "W 1f 00"}, "42607 off");
    expectLog("os_log: SwitchMotion: off", "42607 off");
    check(!dataReady()->enabled && !IOTimerEventSource::last->armed, "42607: idle again");

    // System sleep and wake: the part off, SPI4 clock gated; on wake the pin set-up again and the client's rate back.
    setInterval(kSwitchHIDAccelerometer, 40000);
    expectOps({"W 20 0b", "W 21 2b", "W 23 37", "W 24 47", "W 1f 03", "W 2d 08"}, "42607 accel 25 Hz");
    switch_motion_suspend();
    expectOps({"W 2d 00", "W 20 0c", "W 21 2c", "W 23 37", "W 24 47", "W 1f 00"}, "42607 suspend");
    switch_motion_resume();
    expectOps({"R 36", "W 36 41", "W 05 05", "R 35", "W 35 73", "W 06 38", "W 2b 00", "W 2d 00", "W 20 0c", "W 21 2c",
               "W 23 37", "W 24 47", "W 1f 00", "W 20 0b", "W 21 2b", "W 23 37", "W 24 47", "W 1f 03", "W 2d 08"},
              "42607 resume");
    check(gSpiSuspends == 1 && gSpiResumes == 1 && dataReady()->enabled, "42607: SPI4 suspended and resumed");
    expectClean("42607 session");
    teardown();
}

// Left running by an earlier OS: both sensors on, a latched data-ready, an MREG block selected, and ACCEL_DATA_Z1 (0x0f)
// holding 0x69, LSM6DS3H's WHO_AM_I. Still an ICM-42607-P, and the same reset and set-up.
static void icm42607LeftRunning()
{
    Icm42607 chip;
    const int16_t accel[3] = {0, 0, 0x6900}, gyro[3] = {1, 2, 3};
    chip.r[0x1f] = 0x0f, chip.r[0x20] = 0x09, chip.r[0x21] = 0x09, chip.r[0x06] = 0x38, chip.r[0x2d] = 0x08;
    chip.r[0x3a] = 0, chip.r[0x79] = 0x50, chip.r[0x7c] = 0x50;
    chip.produce(accel, gyro);
    check(chip.r[0x0f] == 0x69 && chip.line(), "left running: 0x0f reads 0x69, INT2 high");
    check(start(chip), "left running: started");
    check(gMotion && gMotion->imu == "ICM-42607-P", "left running: identified by 0x75, not 0x0f");
    expectOps(k42607Start, "left running start");
    check(chip.r[0x1f] == 0 && chip.r[0x79] == 0 && chip.r[0x7c] == 0 && !chip.line(), "left running: reset");
    expectClean("left running");
    teardown();
}

static void icm42607Faults()
{
    {  // A part that never raises data-ready: one line, motion off for the boot, no panic.
        Icm42607 chip;
        check(start(chip), "no data-ready: started");
        setInterval(kSwitchHIDAccelerometer, 10000);
        gChip->ops.clear();
        IOTimerEventSource::last->fire();
        expectLog("IOLog: SwitchMotion: off: ICM-42607-P raised no data-ready on PX2 in 1000 ms at 100.0 Hz (accel 1, "
                  "gyro 0): INT2 not wired or the part stopped",
                  "no data-ready");
        expectOps({"W 2d 00", "W 20 0c", "W 21 2c", "W 23 37", "W 24 47", "W 1f 00"}, "no data-ready: part off");
        check(!dataReady()->enabled && !IOTimerEventSource::last->armed, "no data-ready: masked, no watchdog");
        setInterval(kSwitchHIDGyro, 5000);
        expectOps({}, "no data-ready: off for the boot");
        expectClean("no data-ready");
        teardown();
    }
    {  // INT2 high but the interrupt never arrived: the lost-interrupt panic.
        Icm42607 chip;
        check(start(chip), "lost interrupt: started");
        setInterval(kSwitchHIDAccelerometer, 10000);
        const int16_t accel[3] = {1, 2, 3}, none[3] = {};
        chip.produce(accel, none);
        std::string message;
        try {
            IOTimerEventSource::last->fire();
        } catch (const Panic &p) {
            message = p.what();
        }
        check(message == "SwitchMotion: GPIO 186 interrupt lost (stub)", "lost interrupt: panics (" + message + ")");
        teardown();
    }
    {  // INT2 stuck high with no data: 64 empty interrupts allowed, the 65th turns motion off.
        Icm42607 chip;
        check(start(chip), "stuck: started");
        setInterval(kSwitchHIDAccelerometer, 10000);
        chip.stuck = true;
        check(deliver(100) == 65, "stuck: off after 65 empty interrupts");
        expectLog("IOLog: SwitchMotion: off: ICM-42607-P data-ready (PX2) fired 65 times in a row with no new data: "
                  "GPIO 186 (stub)",
                  "stuck");
        check(!dataReady()->enabled, "stuck: masked");
        teardown();
    }
    {  // A reset that never completes: one line with the registers, nothing started.
        Icm42607 chip;
        chip.resetBroken = true;
        check(!start(chip), "reset fails: not started");
        expectOps({"R 75", "R 0f", "W 7c 00", "W 79 00", "W 02 10", "W 01 04", "R 3a", "R 01", "R 75"}, "reset fails");
        expectLog("IOLog: SwitchMotion: off: ICM-42607-P software reset did not complete (INT_STATUS 0, DEVICE_CONFIG "
                  "0x4, WHO_AM_I now 0x60)",
                  "reset fails");
        check(!gMotion && gHid.empty(), "reset fails: nothing published");
        teardown();
    }
    {  // ICM-42607 (WHO_AM_I 0x67, also the ICM-42670-P): the same map.
        Icm42607 chip(0x67);
        check(start(chip), "0x67: started");
        check(gMotion && gMotion->imu == "ICM-42607", "0x67: ICM-42607");
        expectOps(k42607Start, "0x67 start");
        teardown();
    }
    {  // SPI4 fails at the first WHO_AM_I read: one line, no further transfer, nothing started, no panic.
        Icm42607 chip;
        gSpiFailure = TEGRA_SPI_EIO;
        check(!start(chip), "transfer fails at identify: not started");
        check(gSpiFailed == 1, "transfer fails at identify: one attempt");
        expectLog("IOLog: SwitchMotion: off: SPI4 read of IMU register 0x75 failed (-5)", "transfer fails at identify");
        check(!logged("no IMU on SPI4"), "transfer fails at identify: not taken for an unknown part");
        check(!gMotion && gHid.empty(), "transfer fails at identify: nothing published");
        teardown();
    }
    {  // SPI4 fails while sampling: one line, the part given up, data-ready masked, no further transfer, no panic.
        Icm42607 chip;
        check(start(chip), "transfer fails sampling: started");
        setInterval(kSwitchHIDAccelerometer, 10000);
        const int16_t accel[3] = {1, 2, 3}, none[3] = {};
        chip.produce(accel, none);
        gSpiFailure = TEGRA_SPI_ETIMEDOUT;
        check(deliver() == 1, "transfer fails sampling: one interrupt");
        expectLog("IOLog: SwitchMotion: off: SPI4 read of ICM-42607-P register 0x39 failed (-4)",
                  "transfer fails sampling");
        check(gSpiFailed == 1, "transfer fails sampling: one attempt");
        check(!dataReady()->enabled && !IOTimerEventSource::last->armed,
              "transfer fails sampling: masked, no watchdog");
        setInterval(kSwitchHIDGyro, 5000);
        check(gSpiFailed == 1, "transfer fails sampling: off for the boot");
        teardown();
    }
    {  // Nothing known: both WHO_AM_I values in one line, not a single write.
        UnknownPart chip;
        check(!start(chip), "unknown: not started");
        expectOps({"R 75", "R 0f"}, "unknown: reads only");
        expectLog("IOLog: SwitchMotion: off: no IMU on SPI4 CS0: WHO_AM_I 0x75 = 0x12, 0x0f = 0x34 (expected "
                  "ICM-42607-P 0x60, ICM-42607 0x67, ICM-40607 0x38 at 0x75; LSM6DS3H 0x69, LSM6DSE 0x6a, LSM6DSO 0x6c "
                  "at 0x0f); SCK pad GPIO 21 (stub)",
                  "unknown");
        expectClean("unknown");
        teardown();
    }
}

// FIFO batching: CoreMotion's gyroscope at 200 Hz with a 15 ms batch interval, then its accelerometer joins, then the
// batch interval goes; and the checks that keep data-ready.
static void icm42607Fifo()
{
    const std::vector<std::string> fifo3 = {"R 00",    "R 35",    "W 28 01", "W 29 03", "W 2a 00", "W 7a 01",
                                            "W 7b 23", "W 7d 01", "R 7e",    "W 28 00", "W 2d 04"};
    const int16_t accel[3] = {100, -200, 4096}, gyro[3] = {164, -328, 16};
    {
        Icm42607 chip;
        check(start(chip), "FIFO: started");
        expectOps(k42607Start, "FIFO start");
        setFeature(kSwitchHIDGyro, 5000, 15000);
        std::vector<std::string> want = {"W 2d 00", "W 20 08", "W 21 28", "W 23 34", "W 24 44", "W 1f 0c"};
        want.insert(want.end(), fifo3.begin(), fifo3.end());
        expectOps(want, "FIFO gyro 200 Hz, 15 ms batches: sensors, then the FIFO from empty, then FIFO_THS on INT2");
        expectLog(
            "os_log: SwitchMotion: 200000 mHz, accel 0 (0 us), gyro 1 (5000 us), batch 15000 us, FIFO watermark 3",
            "FIFO gyro");
        gNow += 80000000;  // past the gyroscope's settle time
        chip.produce(accel, gyro);
        gNow += 5000000;
        chip.produce(accel, gyro);
        check(!chip.line() && deliver() == 0, "FIFO: no interrupt below the watermark");
        gNow += 5000000;
        chip.produce(accel, gyro);
        check(deliver() == 1, "FIFO: one interrupt for three samples");
        expectOps({"R 3a x5", "R 3f x48"}, "FIFO drain: status and count in one burst, three records in another");
        expectReports(kSwitchHIDGyro, 3, -174532, 349065, 17027, "FIFO gyro reports");
        const std::vector<uint64_t> spaced = {gNow - 10000000, gNow - 5000000, gNow};
        check(hid(kSwitchHIDGyro)->times == spaced, "FIFO: each sample keeps its time, 5 ms apart, the last now");
        expectLog("IOLog: SwitchMotion: ICM-42607-P FIFO batching on: watermark 3 at 200000 mHz, first drain 3 of 3 "
                  "records (header 0x28)",
                  "FIFO first drain");
        check(!chip.line(), "FIFO: INT2 low after the drain");

        // A late drain: five records, all posted, times still going forward and never past now.
        for (unsigned i = 0; i < 5; ++i) {
            gNow += 5000000;
            chip.produce(accel, gyro);
        }
        check(deliver() == 1, "FIFO: late drain");
        expectOps({"R 3a x5", "R 3f x48", "R 3f x32"}, "FIFO late drain: whole records, three a transfer");
        const std::vector<uint64_t> &times = hid(kSwitchHIDGyro)->times;
        bool forward = times.size() == 8 && times.back() <= gNow;
        for (size_t i = 1; i < times.size(); ++i)
            forward = forward && times[i] > times[i - 1];
        check(forward, "FIFO: times only go forward and stop at now");

        // The accelerometer joins at 50 Hz with 20 ms batches: still 15 ms, both sensors' records, accel 4:1.
        setFeature(kSwitchHIDAccelerometer, 20000, 20000);
        want = {"W 2d 00", "W 20 08", "W 21 28", "W 23 34", "W 24 44", "W 1f 0f"};
        want.insert(want.end(), fifo3.begin(), fifo3.end());
        expectOps(want, "FIFO accel joins");
        gNow += 30000000;  // past the accelerometer's settle time
        hid(kSwitchHIDGyro)->reports.clear(), hid(kSwitchHIDGyro)->times.clear();
        for (unsigned i = 0; i < 12; ++i) {
            gNow += 5000000;
            chip.produce(accel, gyro);
            deliver();
        }
        gChip->ops.clear();
        expectReports(kSwitchHIDGyro, 12, -174532, 349065, 17027, "FIFO both: gyro");
        expectReports(kSwitchHIDAccelerometer, 3, 24414, -48828, -1000000, "FIFO both: accel decimated 4:1");

        // The gyroscope's batch interval goes to 0: data-ready again, the sensors untouched.
        setFeature(kSwitchHIDGyro, 5000, 0);
        expectOps({"W 2d 00", "W 28 01", "W 2d 08"}, "FIFO batch gone: FIFO bypassed, data-ready on INT2");
        expectLog("os_log: SwitchMotion: batch 0 us, FIFO watermark 1", "FIFO batch gone");
        gNow += 5000000;
        chip.produce(accel, gyro);
        check(deliver() == 1, "FIFO off: one interrupt per sample");
        expectOps({"R 39", "R 0b x12"}, "FIFO off: the data-ready path");
        check(hid(kSwitchHIDGyro)->times.back() == 0, "FIFO off: posted as before");

        setInterval(kSwitchHIDGyro, 0);
        setInterval(kSwitchHIDAccelerometer, 0);
        gChip->ops.clear();
        expectClean("FIFO session");
        teardown();
    }
    {  // FIFO_CONFIG5 does not take the write: one line, data-ready for the boot.
        Icm42607 chip;
        chip.mregBroken = true;
        check(start(chip), "MREG: started");
        gChip->ops.clear();
        setFeature(kSwitchHIDGyro, 5000, 15000);
        expectOps({"W 2d 00", "W 20 08", "W 21 28", "W 23 34", "W 24 44", "W 1f 0c", "R 00", "R 35", "W 28 01",
                   "W 29 03", "W 2a 00", "W 7a 01", "W 7b 23", "W 7d 01", "R 7e", "W 2d 08"},
                  "MREG: read back differs, data-ready routed");
        expectLog(
            "IOLog: SwitchMotion: ICM-42607-P FIFO batching off for this boot (FIFO_CONFIG5 (MREG1) read back 0x20 "
            "after writing 0x23); data-ready per sample as before",
            "MREG");
        setFeature(kSwitchHIDAccelerometer, 20000, 20000);
        expectOps({"W 20 08", "W 21 28", "W 23 34", "W 24 44", "W 1f 0f", "W 2d 08"}, "MREG: no FIFO for the boot");
        expectClean("MREG");
        teardown();
    }
    {  // A count past the FIFO: one line, data-ready.
        Icm42607 chip;
        chip.countBroken = true;
        check(start(chip), "count: started");
        setFeature(kSwitchHIDGyro, 5000, 15000);
        gNow += 80000000;
        for (unsigned i = 0; i < 3; ++i)
            chip.produce(accel, gyro);
        gChip->ops.clear();
        check(deliver() == 2, "count: the FIFO interrupt, then data-ready's for the latest sample");
        expectOps({"R 3a x5", "W 2d 00", "W 28 01", "W 2d 08", "R 39", "R 0b x12"},
                  "count: FIFO bypassed, data-ready routed and taken");
        expectLog("IOLog: SwitchMotion: ICM-42607-P FIFO batching off for this boot (FIFO count 65535 past its 144 "
                  "records (INT_STATUS 0x4)); data-ready per sample as before",
                  "count");
        check(hid(kSwitchHIDGyro)->reports.size() == 1 && hid(kSwitchHIDGyro)->times.back() == 0,
              "count: nothing posted from the FIFO, the data-ready sample as before");
        teardown();
    }
    {  // HID reports without sample times: batching off from the start.
        gTimedPosts = false;
        Icm42607 chip;
        check(start(chip), "untimed: started");
        expectLog("IOLog: SwitchMotion: ICM-42607-P FIFO batching off: HID reports cannot carry sample times here "
                  "(SwitchHID); data-ready per sample as before",
                  "untimed");
        gChip->ops.clear();
        setFeature(kSwitchHIDGyro, 5000, 15000);
        expectOps({"W 20 08", "W 21 28", "W 23 34", "W 24 44", "W 1f 0c", "W 2d 08"}, "untimed: data-ready");
        teardown();
    }
}

static void icm40607Session()
{
    Icm40607 chip;
    check(start(chip), "40607: started");
    check(gMotion && gMotion->imu == "ICM-40607", "40607: identified");
    expectOps({"R 75", "R 0f", "W 11 01", "R 2d", "W 16 00", "R 13", "W 13 05", "R 4c", "W 4c 33", "W 14 38", "R 64",
               "W 64 00", "W 65 00", "W 68 00", "W 4f 0b", "W 50 2b", "W 4e 20"},
              "40607 start (no bank-1 INTF_CONFIG4 write)");
    setInterval(kSwitchHIDAccelerometer, 10000);
    expectOps({"W 4f 08", "W 50 28", "W 4e 23", "W 68 08"}, "40607 accel 100 Hz");
    gNow += 25000000;
    const int16_t accel[3] = {100, -200, 4096}, none[3] = {};
    chip.produce(accel, none);
    check(deliver() == 1, "40607: one interrupt per sample");
    expectOps({"R 1f x15"}, "40607 sample burst");
    expectReports(kSwitchHIDAccelerometer, 1, 24414, -48828, -1000000, "40607 accel report");
    expectClean("40607");
    teardown();
}

// The orientation reports IOHIDFamily took, through 21H461's IOHIDEventDriver and CoreMotion
// (orientation_pipeline_model.h): each report makes one orientation event whose only child carries the report's sample
// time, and CoreMotion takes every one, in order.
static void expectCoreMotionTakesEvery(const std::string &what)
{
    const HidDevice *device = hid(kSwitchHIDOrientation);
    REQUIRE(device);
    orientation_model::EventDriver driver(
        orientation_model::parse(kSwitchOrientationDescriptor, sizeof(kSwitchOrientationDescriptor)));
    orientation_model::Notifier coreMotion;
    std::vector<uint8_t> want;
    for (size_t i = 0; i < device->orientationReports.size(); ++i) {
        const SwitchOrientationReport &r = device->orientationReports[i];
        const std::string which = what + ": report " + std::to_string(i);
        check(r.orientation[i % 2] != 0 && r.orientation[1 - i % 2] == 0, which + " in the other set than the last");
        if (gTimedPosts)
            check(device->orientationTimes[i] == r.timestamp, which + ": the report's time is its own");
        const size_t before = driver.dispatched.size();
        driver.report(reinterpret_cast<const uint8_t *>(&r), sizeof(r), device->orientationTimes[i]);
        check(driver.dispatched.size() == before + 1, which + ": one event");
        for (size_t e = before; e < driver.dispatched.size(); ++e) {
            const orientation_model::Event &event = driver.dispatched[e];
            check(event.type == 10 && event.children.size() == 1, which + ": an orientation event with one child");
            if (event.children.size() != 1)
                continue;
            const orientation_model::Event &child = event.children.front();
            uint64_t stamp = 0;
            if (child.data.size() == sizeof(stamp))
                memcpy(&stamp, child.data.data(), sizeof(stamp));
            check(child.type == 1 && child.page == 0xff15 && child.usage == 2 && child.data.size() == 8 &&
                      stamp == r.timestamp,
                  which + ": the child is the 0xff15 / 2 vendor event with the report's 8-byte time");
        }
        want.push_back((uint8_t)__builtin_ctz(device->orientations[i]));
    }
    for (const orientation_model::Event &event : driver.dispatched)
        coreMotion.onEvent(event);
    const bool taken = coreMotion.delivered == want && coreMotion.stale == 0 && coreMotion.noChildren == 0 &&
                       coreMotion.badChild == 0 && coreMotion.wrongType == 0;
    check(taken, what + ": CoreMotion takes every orientation, none stale");
    if (!taken)
        for (const std::string &line : coreMotion.log)
            std::fprintf(stderr, "  CoreMotion: %s\n", line.c_str());
    for (size_t i = 1; i < coreMotion.times.size(); ++i)
        check(coreMotion.times[i] > coreMotion.times[i - 1], what + ": sample times strictly increase");
}

// The orientation detector (SwitchMotion.cpp file comment): with the panel lit the accelerometer runs at 25 Hz for it
// alone, in 5-record FIFO batches. The first sample's orientation is announced at once, again once the console comes
// to rest after moving, and a new one as soon as it has held 0.4 s; with a client at 200 Hz the detector still sees
// 25 Hz. Panel off: nothing converts. Panel on: the detector starts afresh and announces its first sample's
// orientation.
static void icm42607Orientation()
{
    const unsigned landscapeRight = 1u << 4, landscapeLeft = 1u << 3, portrait = 1u << 1;
    gDisplayLit = true;
    Icm42607 chip;
    check(start(chip), "orientation: started");
    check(hid(kSwitchHIDOrientation) != nullptr, "orientation: the {0x20, 0x8a} service is published");
    const std::vector<std::string> fifo5 = {"R 00",    "R 35",    "W 28 01", "W 29 05", "W 2a 00", "W 7a 01",
                                            "W 7b 23", "W 7d 01", "R 7e",    "W 28 00", "W 2d 04"};
    std::vector<std::string> want = k42607Start;
    for (const char *op : {"W 2d 00", "W 20 0b", "W 21 2b", "W 23 37", "W 24 47", "W 1f 03"})
        want.push_back(op);
    want.insert(want.end(), fifo5.begin(), fifo5.end());
    expectOps(want, "orientation: the accelerometer alone at 25 Hz, 5-record FIFO batches, FIFO_THS on INT2");
    expectLog("os_log: SwitchMotion: 25000 mHz, accel 1 (0 us), gyro 0 (0 us), batch 200000 us, FIFO watermark 5",
              "orientation start");
    const std::vector<uint8_t> &announced = hid(kSwitchHIDOrientation)->orientations;
    const int16_t none[3] = {};
    // Ticks of the accelerometer at its 25 Hz (40 ms apart), each FIFO_THS delivered: `raw` in the part's axes.
    const auto run = [&](const int16_t raw[3], unsigned ticks) {
        for (unsigned i = 0; i < ticks; ++i) {
            gNow += 40000000;
            chip.produce(raw, none);
            deliver();
        }
        gChip->ops.clear();
    };
    // Handheld landscape, the panel's top on the user's right: gravity along the portrait panel's +X (device frame
    // (raw x, raw y, -raw z), SwitchMotion.cpp deviceReport), UIDeviceOrientationLandscapeRight.
    const int16_t handheld[3] = {4096, 0, 0}, upright[3] = {0, -4096, 0}, flipped[3] = {-4096, 0, 0};
    const int16_t jolt[3] = {4096, 2048, 0};
    run(handheld, 5);
    check(announced == std::vector<uint8_t>({(uint8_t)landscapeRight}),
          "orientation: the first sample's orientation announced at once (the filter seeded from it)");
    expectLog("os_log: SwitchMotion: orientation landscape right (4), was unknown", "orientation first");
    run(handheld, 15);
    check(announced.size() == 1, "orientation: landscape right announced once");
    run(handheld, 25);
    check(announced.size() == 1, "orientation: nothing more while it stays still");
    run(jolt, 1);
    run(handheld, 5);
    check(announced.size() == 1, "orientation: not while it settles after a jolt");
    run(handheld, 10);
    check(announced == std::vector<uint8_t>({(uint8_t)landscapeRight, (uint8_t)landscapeRight}),
          "orientation: the same orientation announced again once it is still after moving");
    run(upright, 30);
    check(announced.size() == 3 && announced[2] == portrait, "orientation: held upright, portrait announced");
    expectLog("os_log: SwitchMotion: orientation portrait (1), was landscape right", "orientation portrait");

    // A client at 200 Hz (gyroscope, 15 ms batches): one rate for both, the detector decimated 8:1 to 25 Hz.
    setFeature(kSwitchHIDGyro, 5000, 15000);
    gChip->ops.clear();
    expectLog("os_log: SwitchMotion: 200000 mHz, accel 1 (0 us), gyro 1 (5000 us), batch 15000 us, FIFO watermark 3",
              "orientation with a gyroscope client");
    const int16_t spin[3] = {0, 0, 0};
    const auto fast = [&](const int16_t raw[3], unsigned ticks) {
        for (unsigned i = 0; i < ticks; ++i) {
            gNow += 5000000;
            chip.produce(raw, spin);
            deliver();
        }
        gChip->ops.clear();
    };
    fast(flipped, 80);  // 0.4 s at 200 Hz: 10 detector samples, too few for the filter and 0.4 s of agreement
    check(announced.size() == 3, "orientation: at 200 Hz the detector still needs about 0.4 s at 25 Hz");
    fast(flipped, 80);
    check(announced.size() == 4 && announced[3] == landscapeLeft, "orientation: turned over, landscape left");
    setFeature(kSwitchHIDGyro, 0, 0);
    gChip->ops.clear();

    // Panel off: nothing converts, the FIFO is bypassed; no orientation reports.
    switch_motion_display_power(false);
    expectOps({"W 2d 00", "W 20 0c", "W 21 2c", "W 23 37", "W 24 47", "W 1f 00", "W 28 01"},
              "orientation: panel off, the part off");
    expectLog("os_log: SwitchMotion: off", "orientation panel off");
    check(!dataReady()->enabled && !IOTimerEventSource::last->armed,
          "orientation: panel off, no interrupt, no watchdog");
    // Panel on: afresh, the first sample's orientation announced although it did not change.
    switch_motion_display_power(true);
    gChip->ops.clear();
    gNow += 30000000;
    run(flipped, 20);
    check(announced.size() == 5 && announced[4] == landscapeLeft,
          "orientation: panel on, landscape left announced again");
    switch_motion_display_power(false);
    gChip->ops.clear();
    expectCoreMotionTakesEvery("orientation");
    expectClean("orientation");
    teardown();
}

// Without report times the FIFO stays off and each sample is its own interrupt; the clock stands still, so every
// announce reads the same mach_absolute_time. The first report is refused, as IOHIDDevice refuses reports until it
// is ready for input reports: the next one uses the same set and still makes an event.
static void icm42607OrientationUntimed()
{
    const unsigned landscapeRight = 1u << 4;
    const unsigned portrait = 1u << 1;
    gDisplayLit = true;
    gTimedPosts = false;
    Icm42607 chip;
    check(start(chip), "untimed orientation: started");
    HidDevice *orientation = hid(kSwitchHIDOrientation);
    orientation->refuse = 1;
    gNow += 30000000;
    const int16_t none[3] = {};
    const int16_t handheld[3] = {4096, 0, 0};
    const int16_t upright[3] = {0, -4096, 0};
    const int16_t jolt[3] = {4096, 2048, 0};
    const auto run = [&](const int16_t raw[3], unsigned samples) {
        for (unsigned i = 0; i < samples; ++i) {
            chip.produce(raw, none);
            deliver();
        }
        gChip->ops.clear();
    };
    run(handheld, 20);
    check(orientation->refused == 1 && orientation->orientations.empty(), "untimed orientation: the first refused");
    run(jolt, 1);
    run(handheld, 15);
    run(jolt, 1);
    run(handheld, 15);
    run(upright, 30);
    check(orientation->orientations ==
              std::vector<uint8_t>({(uint8_t)landscapeRight, (uint8_t)landscapeRight, (uint8_t)portrait}),
          "untimed orientation: landscape right twice, then portrait");
    const std::vector<uint64_t> &times = orientation->orientationTimes;
    check(times.size() == 3 && times[0] == times[2], "untimed orientation: one mach_absolute_time for every report");
    expectCoreMotionTakesEvery("untimed orientation");
    switch_motion_display_power(false);
    gChip->ops.clear();
    expectClean("untimed orientation");
    teardown();
}

static void lsm6ds3hSession()
{
    Lsm6ds3h chip;
    check(start(chip), "LSM6DS3H: started");
    check(gMotion && gMotion->imu == "LSM6DS3H", "LSM6DS3H: identified");
    expectOps({"R 75", "R 0f", "W 12 01", "R 12", "W 0a 00", "W 12 44", "W 13 08", "W 0d 00", "W 0e 00", "W 10 00",
               "W 11 00"},
              "LSM6DS3H start");
    setInterval(kSwitchHIDAccelerometer, 10000);
    expectOps({"W 10 4c", "W 11 00", "W 0e 01"}, "LSM6DS3H accel 104 Hz");
    gNow += 25000000;
    const int16_t accel[3] = {100, -200, -4098}, none[3] = {};
    chip.produce(accel, none);
    check(deliver() == 1, "LSM6DS3H: one interrupt per sample");
    expectOps({"R 1e x16"}, "LSM6DS3H sample burst");
    expectReports(kSwitchHIDAccelerometer, 1, 24400, 48800, -999912, "LSM6DS3H accel report");
    expectClean("LSM6DS3H");
    teardown();
}

// --- The scenarios, in this order in one process, as the harness has always run them: each session tears the driver
// down (teardown()) before the next starts it again. Do not shard or shuffle.
TEST(SwitchMotion, DecodeAndMapping)
{
    decodeAndMapping();
}

TEST(SwitchMotion, Icm42607Session)
{
    icm42607Session();
}

TEST(SwitchMotion, Icm42607LeftRunning)
{
    icm42607LeftRunning();
}

TEST(SwitchMotion, Icm42607Faults)
{
    icm42607Faults();
}

TEST(SwitchMotion, Icm42607Fifo)
{
    icm42607Fifo();
}

TEST(SwitchMotion, Icm42607Orientation)
{
    icm42607Orientation();
}

TEST(SwitchMotion, Icm42607OrientationUntimed)
{
    icm42607OrientationUntimed();
}

TEST(SwitchMotion, Icm40607Session)
{
    icm40607Session();
}

TEST(SwitchMotion, Lsm6ds3hSession)
{
    lsm6ds3hSession();
}
