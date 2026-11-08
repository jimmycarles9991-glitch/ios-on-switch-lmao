// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 GPIO controller as an IOInterruptController with one vector per pin; see TegraGPIO.h.
//
// Register layout, INT_LVL encoding, the masked-write registers and the bank handler's order (acknowledge each
// pending pin in INT_CLR, then run its handler) follow Linux drivers/gpio/gpio-tegra.c (GPIO_* offsets lines 28-59,
// tegra_gpio_mask_write 120-129, tegra_gpio_set_debounce 226-258, tegra_gpio_irq_set_type 300-345,
// tegra_gpio_irq_handler 378-430, tegra210_gpio_config 813-816: bank stride 0x100, masked registers at +0x80).
// The vector protocol (interruptActive, soft disable from the client, lazy hard disable here) is the one
// IOInterruptController::enableInterrupt/disableInterrupt expect (iokit/Kernel/IOInterruptController.cpp).
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOInterruptController.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include "TegraGPIO.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr vm_offset_t kGpioPA = 0x6000D000, kPinmuxPA = 0x70003000, kCarPA = 0x60006000;
// CAR (hekate bdk/soc/clock.h): RST_DEVICES_L 0x004, CLK_ENB_L_SET 0x320, RST_DEV_L_CLR 0x304, GPIO = bit 8.
constexpr uint32_t kCarRstDevicesL = 0x004, kCarClkEnbLSet = 0x320, kCarRstDevLClr = 0x304, kCarGpio = 1u << 8;
constexpr unsigned kBanks = 8;
// Bank SPIs: Linux tegra210.dtsi:491-498; OLED DTB gpio@6000d000 interrupts; hekate bdk/soc/gpio.c:43-50.
constexpr uint32_t kBankSpi[kBanks] = {32, 33, 34, 35, 55, 87, 89, 125};

// Per-port registers at bank * 0x100 + port * 4; the masked copies (bits 15:8 select, 7:0 value) at +0x80.
enum : uint32_t {
    CNF = 0x00,
    OE = 0x10,
    OUT = 0x20,
    IN = 0x30,
    INT_STA = 0x40,
    INT_ENB = 0x50,
    INT_LVL = 0x60,
    INT_CLR = 0x70,
    DBC_CNT = 0xF0,
    MSK = 0x80,
    MSK_CNF = MSK + CNF,
    MSK_OE = MSK + OE,
    MSK_OUT = MSK + OUT,
    MSK_DBC_EN = MSK + 0x30,
    MSK_INT_ENB = MSK + INT_ENB,
};

// INT_LVL fields per pin: bit n level/edge polarity, bit n+8 edge, bit n+16 both edges.
constexpr uint32_t kLvlMask = 0x010101;

// Pinmux (PINMUX_AUX_x at 0x70003000): function 1:0, pull 3:2, tristate 4, input enable 6 (hekate bdk/soc/pinmux.h).
enum : uint32_t {
    PULL_MASK = 3u << 2,
    PULL_NONE = 0,
    PULL_DOWN = 1u << 2,
    PULL_UP = 2u << 2,
    TRISTATE = 1u << 4,
    INPUT = 1u << 6,
};

struct PinMux {
    uint16_t gpio, offset;
    uint32_t pull;
};

// tegra210b01-fric-pinmux.dtsi (switch-l4t-platform-t210-nx), the /* 32xx */ comment of each node.
constexpr PinMux kPinMux[] = {
    {TEGRA_GPIO_PX1, 0x220, PULL_UP},    // touch_int_px1, lines 981-987
    {TEGRA_GPIO_PX2, 0x224, PULL_DOWN},  // motion_int_px2, 955-961
    {TEGRA_GPIO_PX4, 0x22c, PULL_UP},    // temp_alert_px4, 946-952
    {TEGRA_GPIO_PX6, 0x234, PULL_UP},    // button_vol_up_px6, 1007-1012
    {TEGRA_GPIO_PX7, 0x238, PULL_UP},    // button_vol_down_px7, 1016-1021
    {TEGRA_GPIO_PY0, 0x23c, PULL_NONE},  // button_slide_sw_py0, 1246-1251
    {TEGRA_GPIO_PZ0, 0x27c, PULL_NONE},  // pz0, 1061-1066
    {TEGRA_GPIO_PK4, 0x264, PULL_UP},    // pk4, 1079-1084
};

// Output pins, whole register: function, pull, driven (not tristate), input disabled.
struct OutputMux {
    uint16_t gpio, offset;
    uint32_t value;
};

constexpr OutputMux kOutputMux[] = {
    {TEGRA_GPIO_PJ7, 0x150, PULL_DOWN | 1},    // dap4_sclk_pj7 "Touchscreen Reset": rsvd1, pull-down, 476-481;
                                               // hekate touch_power_on writes PINMUX_AUX_DAP4_SCLK the same
    {TEGRA_GPIO(X, 3), 0x228, PULL_DOWN},      // als_prox_int_px3: v_vdd5v3 enable on Fric (USB-C VBUS source),
                                               // rsvd0, pull-down, 937-943 (drivers/SwitchUSB SwitchPD)
    {TEGRA_GPIO(K, 5), 0x268, PULL_DOWN | 1},  // pk5 "USB-PD (BM92T36) VCONN Power Enable": rsvd1, 1087-1094
};

inline uint32_t portReg(unsigned gpio, uint32_t reg)
{
    return (gpio >> 5) * 0x100 + ((gpio >> 3) & 3) * 4 + reg;
}

inline uint32_t pinBit(unsigned gpio)
{
    return 1u << (gpio & 7);
}

inline bool isLevel(uint8_t trigger)
{
    return trigger == TEGRA_GPIO_LEVEL_HIGH || trigger == TEGRA_GPIO_LEVEL_LOW;
}

uint32_t lvlBits(uint8_t trigger)
{
    switch (trigger) {
    case TEGRA_GPIO_EDGE_RISING:
        return 0x000101;
    case TEGRA_GPIO_EDGE_FALLING:
        return 0x000100;
    case TEGRA_GPIO_EDGE_BOTH:
        return 0x010100;
    case TEGRA_GPIO_LEVEL_HIGH:
        return 0x000001;
    default:
        return 0;  // level low
    }
}

constexpr char kPortNames[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class TegraGPIO : public IOInterruptController {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(TegraGPIO)

        volatile uint8_t *gpio = nullptr,
                         *pinmux = nullptr;
    volatile uint8_t *car = nullptr;  // sleep resume only (mapped at start: resume runs with interrupts off)
    IOWorkLoop *loop = nullptr;
    IOFilterInterruptEventSource *banks[kBanks] = {};
    IOLock *nubLock = nullptr;
    IOSimpleLock *portLock = nullptr;  // INT_LVL and DBC_CNT read-modify-write
    IOService *nubs[TEGRA_GPIO_COUNT] = {};
    uint8_t triggers[TEGRA_GPIO_COUNT] = {};
    uint8_t debounce[TEGRA_GPIO_COUNT / 8] = {};
    uint8_t debouncePins[TEGRA_GPIO_COUNT / 8] = {};  // pins with debounce on, per port (DBC_EN is write-only)

    // System sleep: each port's registers, saved before SC7 (the controller and its clock come back at reset).
    struct Saved {
        uint8_t cnf, oe, out, intEnable;
        uint32_t intLevel;
    };

    Saved saved[TEGRA_GPIO_COUNT / 8] = {};

    uint32_t rd(uint32_t o) const
    {
        return switch_mmio_read32(gpio, o);
    }

    void wr(uint32_t o, uint32_t v)
    {
        switch_mmio_write32(gpio, o, v);
    }

    void maskWrite(unsigned pin, uint32_t reg, bool on)
    {
        wr(portReg(pin, reg), (pinBit(pin) << 8) | (on ? pinBit(pin) : 0));
    }

    static void unused(OSObject *, IOInterruptEventSource *, int)
    {}

    static bool bankInterrupt(OSObject *owner, IOFilterInterruptEventSource *sender);
    void serviceBank(unsigned bank);
    void configurePin(unsigned pin, uint8_t trigger);
    IOService *makeNub(unsigned pin);

public:
    bool start(IOService *provider) override;
    void free() override;
    void suspend();
    void resume();
    int getVectorType(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) override;
    void disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) override;
    void enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) override;

    IOService *nubFor(unsigned pin, unsigned trigger);

    int read(unsigned pin) const
    {
        return (rd(portReg(pin, IN)) & pinBit(pin)) ? 1 : 0;
    }

    int sample(unsigned pin, unsigned *config);

    unsigned config(unsigned pin) const
    {
        const uint32_t bit = pinBit(pin);
        return ((rd(portReg(pin, CNF)) & bit) ? 1u : 0u) | ((rd(portReg(pin, OE)) & bit) ? 2u : 0u) |
               ((rd(portReg(pin, OUT)) & bit) ? 4u : 0u) | ((rd(portReg(pin, IN)) & bit) ? 8u : 0u);
    }

    void describe(unsigned pin, char *buffer, size_t size) const;

    bool latched(unsigned pin) const
    {
        return (rd(portReg(pin, INT_STA)) & rd(portReg(pin, INT_ENB)) & pinBit(pin)) != 0u;
    }

    void setDebounce(unsigned pin, unsigned ms);
    void setOutput(unsigned pin, bool high);
};

OSDefineMetaClassAndStructors(TegraGPIO, IOInterruptController)

    namespace
{
    TegraGPIO *gController;
    const OSSymbol *gControllerName;
}

bool TegraGPIO::start(IOService *provider)
{
    if (!IOInterruptController::start(provider))
        return false;
    gpio = switch_mmio_map(kGpioPA, 0x1000);
    pinmux = switch_mmio_map(kPinmuxPA, 0x1000);
    car = switch_mmio_map(kCarPA, 0x1000);
    nubLock = IOLockAlloc();
    portLock = IOSimpleLockAlloc();
    vectors = IONewZero(IOInterruptVector, TEGRA_GPIO_COUNT);
    if (!gpio || !pinmux || !car || !nubLock || !portLock || !vectors)
        return false;
    for (unsigned i = 0; i < TEGRA_GPIO_COUNT; ++i) {
        vectors[i].interruptLock = IOLockAlloc();
        if (!vectors[i].interruptLock)
            return false;
    }

    // No pin interrupt is ours until a nub is enabled: mask and acknowledge everything the boot chain left.
    for (unsigned port = 0; port < kBanks * 4; ++port) {
        wr(portReg(port * 8, INT_ENB), 0);
        wr(portReg(port * 8, INT_CLR), 0xff);
    }

    loop = IOWorkLoop::workLoop();
    if (!loop)
        return false;
    for (unsigned b = 0; b < kBanks; ++b) {
        IOService *nub = switch_interrupt_nub(32 + kBankSpi[b], false);  // sleeps until the GIC is up
        banks[b] = IOFilterInterruptEventSource::filterInterruptEventSource(this, &TegraGPIO::unused,
                                                                            &TegraGPIO::bankInterrupt, nub, 0);
        if (!banks[b] || loop->addEventSource(banks[b]) != kIOReturnSuccess) {
            panic("TegraGPIO: cannot attach the bank %u interrupt (INTID %u)", b, 32 + kBankSpi[b]);
        }
    }
    for (auto *bank : banks)
        bank->enable();

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): registerInterruptController takes a non-const OSSymbol
    getPlatform()->registerInterruptController(const_cast<OSSymbol *>(gControllerName), this);
    registerService();
    return true;
}

void TegraGPIO::free()
{
    for (auto *&bank : banks) {
        if (!bank)
            continue;
        bank->disable();
        if (loop)
            loop->removeEventSource(bank);
        bank->release();
        bank = nullptr;
    }
    OSSafeReleaseNULL(loop);
    if (vectors) {
        for (unsigned i = 0; i < TEGRA_GPIO_COUNT; ++i) {
            if (vectors[i].interruptLock)
                IOLockFree(vectors[i].interruptLock);
        }
        IODelete(vectors, IOInterruptVector, TEGRA_GPIO_COUNT);
        vectors = nullptr;
    }
    if (portLock)
        IOSimpleLockFree(portLock);
    if (nubLock)
        IOLockFree(nubLock);
    IOInterruptController::free();
}

// Primary interrupt context, one bank's SPI. Everything is handled here, so the filter never wakes the work loop.
bool TegraGPIO::bankInterrupt(OSObject *owner, IOFilterInterruptEventSource *sender)
{
    auto *self = static_cast<TegraGPIO *>(owner);
    for (unsigned b = 0; b < kBanks; ++b) {
        if (self->banks[b] == sender)
            self->serviceBank(b);
    }
    return false;
}

void TegraGPIO::serviceBank(unsigned bank)
{
    for (unsigned port = 0; port < 4; ++port) {
        const unsigned first = bank * 32 + port * 8;
        uint32_t pending = rd(portReg(first, INT_STA)) & rd(portReg(first, INT_ENB)) & 0xff;
        if (!pending)
            continue;
        wr(portReg(first, INT_CLR), pending);
        for (; pending; pending &= pending - 1) {
            const IOInterruptVectorNumber number =
                static_cast<IOInterruptVectorNumber>(first + static_cast<unsigned>(__builtin_ctz(pending)));
            IOInterruptVector *vector = &vectors[number];
            vector->interruptActive = 1;
            OSMemoryBarrier();
            if (vector->interruptRegistered && !vector->interruptDisabledSoft) {
                vector->handler(vector->target, vector->refCon, vector->nub, vector->source);
            }
            // A level client soft-disables itself in its handler (IOInterruptEventSource autoDisable); mask the pin
            // until enableInterrupt() so the line does not fire again while its action runs.
            if (!vector->interruptRegistered || vector->interruptDisabledSoft) {
                vector->interruptDisabledHard = 1;
                disableVectorHard(number, vector);
            }
            OSMemoryBarrier();
            vector->interruptActive = 0;
        }
    }
}

int TegraGPIO::getVectorType(IOInterruptVectorNumber vectorNumber, IOInterruptVector *)
{
    return isLevel(triggers[vectorNumber]) ? kIOInterruptTypeLevel : kIOInterruptTypeEdge;
}

void TegraGPIO::disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *)
{
    maskWrite(static_cast<unsigned>(vectorNumber), MSK_INT_ENB, false);
}

// A level pin was hard-masked while its client ran; whether INT_STA keeps the level latched from before the device
// released the line is not documented, so clear it first: a line still asserted latches again and fires, a
// released one does not fire a ghost. Edge pins are never cleared here (that could drop a real edge).
void TegraGPIO::enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *)
{
    const unsigned pin = static_cast<unsigned>(vectorNumber);
    if (isLevel(triggers[pin]))
        wr(portReg(pin, INT_CLR), pinBit(pin));
    maskWrite(pin, MSK_INT_ENB, true);
}

// GPIO mode, input, trigger; pinmux for the pins this board's drivers use (tegra_gpio_irq_set_type order).
void TegraGPIO::configurePin(unsigned pin, uint8_t trigger)
{
    for (const PinMux &mux : kPinMux) {
        if (mux.gpio != pin)
            continue;
        switch_mmio_rmw32(pinmux, mux.offset, PULL_MASK, mux.pull | TRISTATE | INPUT);
    }
    maskWrite(pin, MSK_INT_ENB, false);
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(portLock);
    const uint32_t shift = pin & 7;
    wr(portReg(pin, INT_LVL), (rd(portReg(pin, INT_LVL)) & ~(kLvlMask << shift)) | (lvlBits(trigger) << shift));
    IOSimpleLockUnlockEnableInterrupt(portLock, state);
    maskWrite(pin, MSK_OE, false);
    maskWrite(pin, MSK_CNF, true);
    wr(portReg(pin, INT_CLR), pinBit(pin));
    triggers[pin] = trigger;
}

// An IOService whose interrupt 0 resolves to this controller's vector `pin`, attached under the controller.
IOService *TegraGPIO::makeNub(unsigned pin)
{
    // IOInterruptVectorNumber, as resolveInterrupt reads the specifier
    const int32_t vector = static_cast<int32_t>(pin);
    const OSObject *controller = gControllerName;
    const OSArray *controllers = OSArray::withObjects(&controller, 1);
    const OSData *specifier = OSData::withBytes(&vector, sizeof(vector));
    const OSObject *specifierObject = specifier;
    const OSArray *specifiers = specifier ? OSArray::withObjects(&specifierObject, 1) : nullptr;
    const OSNumber *number = OSNumber::withNumber(pin, 32);
    OSDictionary *properties = OSDictionary::withCapacity(3);
    IOService *nub = OSTypeAlloc(IOService);
    bool ok = (controllers != nullptr) && (specifiers != nullptr) && (number != nullptr) && (properties != nullptr) &&
              (nub != nullptr);
    if (ok) {
        properties->setObject(gIOInterruptControllersKey, controllers);
        properties->setObject(gIOInterruptSpecifiersKey, specifiers);
        properties->setObject("gpio", number);
        ok = nub->init(properties) && nub->attach(this);
    }
    if (ok) {
        const unsigned port = pin >> 3;
        const char letter = kPortNames[port < 26 ? port : port - 26];
        char name[16];
        if (port < 26) {
            snprintf(name, sizeof(name), "gpio-P%c%u", letter, pin & 7);
        } else {
            snprintf(name, sizeof(name), "gpio-P%c%c%u", letter, letter, pin & 7);
        }
        nub->setName(name);
    }
    OSSafeReleaseNULL(controllers);
    OSSafeReleaseNULL(specifier);
    OSSafeReleaseNULL(specifiers);
    OSSafeReleaseNULL(number);
    OSSafeReleaseNULL(properties);
    IOService *result = ok ? nub : nullptr;
    OSSafeReleaseNULL(nub);  // on success the attachment keeps it
    return result;
}

IOService *TegraGPIO::nubFor(unsigned pin, unsigned trigger)
{
    IOLockLock(nubLock);
    IOService *nub = nubs[pin];
    if (nub && triggers[pin] != trigger) {
        if (vectors[pin].interruptRegistered) {
            IOLockUnlock(nubLock);
            IOLog("TegraGPIO: GPIO %u is in use with trigger %u\n", pin, triggers[pin]);
            return nullptr;
        }
        configurePin(pin, static_cast<uint8_t>(trigger));
    }
    if (!nub) {
        nub = makeNub(pin);
        if (nub) {
            configurePin(pin, static_cast<uint8_t>(trigger));
            nubs[pin] = nub;
        } else {
            IOLog("TegraGPIO: nub for GPIO %u failed\n", pin);
        }
    }
    IOLockUnlock(nubLock);
    return nubs[pin];
}

void TegraGPIO::setDebounce(unsigned pin, unsigned ms)
{
    if (!ms) {
        debouncePins[pin >> 3] &= static_cast<uint8_t>(~pinBit(pin));
        maskWrite(pin, MSK_DBC_EN, false);
        return;
    }
    if (ms > 255)
        ms = 255;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(portLock);
    if (debounce[pin >> 3] < ms) {
        debounce[pin >> 3] = static_cast<uint8_t>(ms);
        wr(portReg(pin, DBC_CNT), ms);
    }
    debouncePins[pin >> 3] |= static_cast<uint8_t>(pinBit(pin));
    IOSimpleLockUnlockEnableInterrupt(portLock, state);
    maskWrite(pin, MSK_DBC_EN, true);
}

// System sleep (SwitchSuspend quiesce/active actions: boot CPU, interrupts off, the other cores off; pinmux is
// restored before resume). Linux gpio-tegra.c tegra_gpio_suspend/_resume: configuration, levels and triggers first,
// stale status cleared, interrupt enables last. The controller's clock (CLK_L bit 8, as hekate's clock_enable_gpio)
// is turned on and its reset released after SC7 before anything is written.
void TegraGPIO::suspend()
{
    for (unsigned port = 0; port < kBanks * 4; ++port) {
        const unsigned pin = port * 8;
        saved[port] = {static_cast<uint8_t>(rd(portReg(pin, CNF))), static_cast<uint8_t>(rd(portReg(pin, OE))),
                       static_cast<uint8_t>(rd(portReg(pin, OUT))), static_cast<uint8_t>(rd(portReg(pin, INT_ENB))),
                       rd(portReg(pin, INT_LVL))};
    }
}

void TegraGPIO::resume()
{
    switch_mmio_write32(car, kCarClkEnbLSet, kCarGpio);
    switch_mmio_write32(car, kCarRstDevLClr, kCarGpio);
    (void)switch_mmio_read32(car, kCarRstDevicesL);
    for (unsigned port = 0; port < kBanks * 4; ++port) {
        const unsigned pin = port * 8;
        wr(portReg(pin, INT_ENB), 0);
        wr(portReg(pin, OUT), saved[port].out);
        wr(portReg(pin, OE), saved[port].oe);
        wr(portReg(pin, CNF), saved[port].cnf);
        wr(portReg(pin, INT_LVL), saved[port].intLevel);
        if (debounce[port]) {
            wr(portReg(pin, DBC_CNT), debounce[port]);
            wr(portReg(pin, MSK_DBC_EN), 0xff00u | debouncePins[port]);
        }
        wr(portReg(pin, INT_CLR), 0xff);
    }
    for (unsigned port = 0; port < kBanks * 4; ++port)
        wr(portReg(port * 8, INT_ENB), saved[port].intEnable);
    __asm__ volatile("dsb sy" ::: "memory");
}

extern "C" void tegra_gpio_suspend(void)
{
    if (gController)
        gController->suspend();
}

extern "C" void tegra_gpio_resume(void)
{
    if (gController)
        gController->resume();
}

// Pinmux, then the level before the driver turns on, then GPIO mode (hekate gpio_direction_output order). The masked
// registers make each write atomic per pin, so no lock is needed.
void TegraGPIO::setOutput(unsigned pin, bool high)
{
    for (const OutputMux &mux : kOutputMux) {
        if (mux.gpio == pin)
            switch_mmio_write32(pinmux, mux.offset, mux.value);
    }
    maskWrite(pin, MSK_OUT, high);
    maskWrite(pin, MSK_OE, true);
    maskWrite(pin, MSK_CNF, true);
}

// The output enable goes off before the pin leaves its special function, and comes back after it returns, so the pad
// is released throughout. Masked writes: atomic per pin.
int TegraGPIO::sample(unsigned pin, unsigned *config)
{
    const uint32_t bit = pinBit(pin);
    const bool cnf = (rd(portReg(pin, CNF)) & bit) != 0u;
    const bool oe = (rd(portReg(pin, OE)) & bit) != 0u;
    if (config)
        *config = this->config(pin);
    maskWrite(pin, MSK_OE, false);
    maskWrite(pin, MSK_CNF, true);
    IODelay(2);
    const int level = (rd(portReg(pin, IN)) & bit) ? 1 : 0;
    maskWrite(pin, MSK_CNF, cnf);
    maskWrite(pin, MSK_OE, oe);
    return level;
}

// One line for a consumer's panic: the pin's level and interrupt registers, then its bank's GIC state.
void TegraGPIO::describe(unsigned pin, char *buffer, size_t size) const
{
    char gic[160];
    const uint32_t bit = pinBit(pin);
    switch_interrupt_describe(32 + kBankSpi[pin >> 5], gic, sizeof(gic));
    snprintf(buffer, size, "GPIO %u: cnf %u oe %u in %u int_enb %u int_sta %u int_lvl %#x trigger %u, bank %u: %s", pin,
             static_cast<int>((rd(portReg(pin, CNF)) & bit) != 0), static_cast<int>((rd(portReg(pin, OE)) & bit) != 0),
             static_cast<int>((rd(portReg(pin, IN)) & bit) != 0),
             static_cast<int>((rd(portReg(pin, INT_ENB)) & bit) != 0),
             static_cast<int>((rd(portReg(pin, INT_STA)) & bit) != 0),
             (rd(portReg(pin, INT_LVL)) >> (pin & 7)) & kLvlMask, triggers[pin], pin >> 5, gic);
}

extern "C" int tegra_gpio_start(void)
{
    static IOLock *volatile startLock;
    if (!startLock) {
        IOLock *fresh = IOLockAlloc();
        if (fresh && !OSCompareAndSwapPtr(nullptr, fresh, &startLock))
            IOLockFree(fresh);
    }
    IOLock *lock = startLock;
    if (!lock)
        return -1;
    IOLockLock(lock);
    int result = 0;
    if (!gController) {
        if (!gControllerName)
            gControllerName = OSSymbol::withCString("TegraGPIO");
        IOService *resources = IOService::getResourceService();
        TegraGPIO *controller = OSTypeAlloc(TegraGPIO);
        result = -1;
        if (gControllerName && resources && controller && controller->init() && controller->attach(resources)) {
            if (controller->start(resources)) {
                gController = controller;
                result = 0;
                IOLog("TegraGPIO: started\n");
            } else {
                controller->detach(resources);
            }
        } else {
            IOLog("TegraGPIO: attach failed\n");
        }
        OSSafeReleaseNULL(controller);  // on success the attachment keeps it
    }
    IOLockUnlock(lock);
    return result;
}

extern "C" IOService *tegra_gpio_interrupt_nub(unsigned gpio, unsigned trigger)
{
    if (gpio >= TEGRA_GPIO_COUNT || (!gController && tegra_gpio_start()))
        return nullptr;
    switch (trigger) {
    case TEGRA_GPIO_EDGE_RISING:
    case TEGRA_GPIO_EDGE_FALLING:
    case TEGRA_GPIO_EDGE_BOTH:
    case TEGRA_GPIO_LEVEL_HIGH:
    case TEGRA_GPIO_LEVEL_LOW:
        return gController->nubFor(gpio, trigger);
    default:
        return nullptr;
    }
}

extern "C" int tegra_gpio_read(unsigned gpio)
{
    return gController && gpio < TEGRA_GPIO_COUNT ? gController->read(gpio) : -1;
}

extern "C" int tegra_gpio_sample(unsigned gpio, unsigned *config)
{
    return gController && gpio < TEGRA_GPIO_COUNT ? gController->sample(gpio, config) : -1;
}

extern "C" int tegra_gpio_config(unsigned gpio)
{
    return gController && gpio < TEGRA_GPIO_COUNT ? (int)gController->config(gpio) : -1;
}

extern "C" void tegra_gpio_describe(unsigned gpio, char *buffer, size_t size)
{
    if (!size)
        return;
    if (gController && gpio < TEGRA_GPIO_COUNT) {
        gController->describe(gpio, buffer, size);
    } else {
        snprintf(buffer, size, "GPIO %u: controller not started", gpio);
    }
}

// The bank filter acknowledges a pending, enabled pin within microseconds, so one still latched after 50 ms never
// reached the CPU.
extern "C" void tegra_gpio_check_delivery(unsigned gpio, const char *who)
{
    if (!gController || gpio >= TEGRA_GPIO_COUNT || !gController->latched(gpio))
        return;
    IOSleep(50);
    if (!gController->latched(gpio))
        return;
    char state[256];
    gController->describe(gpio, state, sizeof(state));
    panic("%s: GPIO %u interrupt lost: %s", who, gpio, state);
}

extern "C" int tegra_gpio_set_debounce(unsigned gpio, unsigned ms)
{
    if (!gController || gpio >= TEGRA_GPIO_COUNT)
        return -1;
    gController->setDebounce(gpio, ms);
    return 0;
}

extern "C" int tegra_gpio_output(unsigned gpio, int value)
{
    if (gpio >= TEGRA_GPIO_COUNT || (!gController && tegra_gpio_start()))
        return -1;
    gController->setOutput(gpio, value != 0);
    return 0;
}
