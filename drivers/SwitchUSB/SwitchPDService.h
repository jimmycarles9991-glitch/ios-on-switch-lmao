// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchPD's service (SwitchPD.cpp: the power role; SwitchPDRequest.cpp: the USB-PD request; SwitchPDSettings.cpp:
// the controller's settings this boot changes, and their return) and the BM92T36's registers and bus, which all use.
#pragma once
#include "SwitchPD.h"
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <os/log.h>
#include "../SwitchBattery/SwitchBattery.h"
#include "../SwitchBattery/SwitchChargePolicy.h"
#include "../SwitchDisplayPort/SwitchPDAltModeHook.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraI2C/TegraI2C.h"
#include "SwitchPDPolicy.h"

namespace switchpd {
constexpr unsigned kBus = 1;
constexpr uint8_t kPD = 0x18;
constexpr unsigned kVconnGpio = TEGRA_GPIO(K, 5), kSourceRailGpio = TEGRA_GPIO(X, 3);
// The BQ24193's PSEL and OTG inputs (L4T tegra210b01-fric-gpio.dtsi "Charger IC (BQ24193) Charge Select 1/2", held
// low; Atmosphere's OtgFet1/2ForSdev): the boost runs only with OTG high (datasheet 8.3.1.5). Read for the report.
[[maybe_unused]] constexpr unsigned kChargeSelect1Gpio = TEGRA_GPIO(J, 5), kChargeSelect2Gpio = TEGRA_GPIO(L, 0);
constexpr unsigned kMaxListeners = 4;
constexpr unsigned kAlertBurst = 50;  // ALERTs in one second that mean the line is stuck low
constexpr uint32_t kRailRampMS = 10;  // v_vdd5v3 regulator-enable-ramp-delay (10000 us)
constexpr uint32_t kClassifyMS = 1000, kWaitSourceMS = 800;
constexpr uint32_t kPrSwapTimeoutMS = 500, kPrSwapRetryMS = 150;
// bm92txx.c's msleep()s around SYS_RESET: 100 ms at start, 33 ms after a fault, 100 ms before the next event's work.
constexpr uint32_t kStartResetMS = 100, kFaultResetMS = 33, kAfterResetMS = 100;
// USB-PD request: source PDOs after a contract alert without ALERT_PDO (bm92t_event_handler's msleep(500)), a reply to
// a command (the chip answers in tens of ms; the old in-tree port's kReplyMS), the input limit 2 s after PS_RDY
// (bm92t_power_work), failed requests per session.
constexpr uint32_t kPdoWaitMS = 500, kReplyMS = 1500, kPowerDelayMS = 2000;
constexpr unsigned kMaxFailedRequests = 3;

enum : uint8_t {
    ALERT_STATUS = 0x02,  // clears on read; reading it releases ALERT
    STATUS1 = 0x03,
    STATUS2 = 0x04,
    COMMAND = 0x05,
    CONFIG1 = 0x06,
    READ_PDOS_SRC = 0x08,  // count byte (4 per PDO), up to 7 PDOs
    CONFIG2 = 0x17,
    DP_STATUS = 0x18,
    DP_ALERT_EN = 0x19,
    VENDOR_CONFIG = 0x1a,
    CURRENT_PDO = 0x28,   // count byte, PDO
    CURRENT_RDO = 0x2b,   // count byte, RDO
    ALERT_ENABLE = 0x2e,  // four bytes, no count byte (bm92txx bm92t_regs_show)
    SET_RDO = 0x30,       // count byte, RDO
    FW_TYPE = 0x4b,
    FW_REVISION = 0x4c,
};

enum : unsigned {
    CONFIG1_SPDSRC_SHIFT = 14,
    CONFIG1_SPDSRC_MASK = 3u << 14,
    SPDSRC12_ON = 0,
    SPDSRC12_OFF = 3,
    VENDOR_CONFIG_OCP_DISABLE = 1u << 2,
    CONFIG2_PR_SWAP_MASK = 3u << 0,
    CONFIG2_PR_SWAP_ACCEPT_SRC_REJECT_SNK = 2u,
    CONFIG2_DR_SWAP_MASK = 3u << 2,
    CONFIG2_DR_SWAP_ALWAYS_ACCEPT = 3u << 2,  // DR_SWAP field 3:2: bit 2 accept as UFP, bit 3 accept as DFP
    CONFIG2_VSRC_SWAP = 1u << 4,
    ALERT_CMD_DONE = 1u << 2,
    ALERT_DR_SWAP = 1u << 10,
    PDO_FIXED_DUAL_ROLE_DATA = 1u << 25,  // the source's 5 V Fixed PDO: USB PD R3.3 V1.0 6.4.1.3.3 Table 6.8
    ALERT_PDO = 1u << 14,
    STATUS1_LASTCMD_SHIFT = 4,  // 0 complete, 2 aborted, 4 invalid, 6 rejected, 7 terminated
    STATUS1_LASTCMD_MASK = 7u << 4,
    STATUS1_CMD_BUSY = 1u << 13,
    LASTCMD_ABORTED = 2,
    PR_SWAP_CMD = 0x0303,
    SEND_RDO_CMD = 0x0707,
    PS_RDY_CMD = 0x0505,
    PD_HARD_RST_CMD = 0x0808,
    SYS_RESET_CMD = 0x0D0D,
};

struct Listener {
    void (*function)(void *, const SwitchPDState *);
    void *context;
};

inline int read16(uint8_t reg, uint16_t &value)
{
    uint8_t b[2] = {};
    const int err = tegra_i2c_read(kBus, kPD, reg, b, 2);
    value = static_cast<uint16_t>(b[0] | static_cast<unsigned>(b[1]) << 8);
    return err;
}

inline int write16(uint8_t reg, uint16_t value)
{
    const uint8_t b[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
    return tegra_i2c_write(kBus, kPD, reg, b, 2);
}

// The USB-PD request within a sink session (SwitchPD.h).
enum class Nego : uint8_t {
    Idle,       // waiting for the controller's own contract
    WaitPdos,   // a contract alert without the source's PDOs yet: kPdoWaitMS
    RdoSent,    // SEND_RDO: waiting for the contract (kReplyMS)
    PsRdySent,  // contract verified, PS_RDY: waiting for it to complete (kReplyMS)
    Settling,   // PS_RDY done: kPowerDelayMS before publishing
    Done,       // published
    NotPd,      // no source capabilities: not a USB-PD source
    Failed,     // one line said why; the controller's own contract stays, nothing published
};
}  // namespace switchpd

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class SwitchPD : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchPD);

    IOWorkLoop *loop = nullptr;
    IOInterruptEventSource *alert = nullptr;
    IOTimerEventSource *window = nullptr, *negoTimer = nullptr;
    switchpd::Listener listeners[switchpd::kMaxListeners] = {};
    unsigned listenerCount = 0;
    SwitchPDState state = {};
    switch_pd_phase phase = SWITCH_PD_EMPTY;
    bool allowSource = false, switchesOn = false, vbusRailOn = false, vconnOn = false, vconnSwap = false,
         sawDetach = false, stuck = false;
    bool reportedReadFailure = false, reportedSwitchFailure = false;
    bool asleepSinkOnly = false;  // SC7 holds an empty or canceled startup sink-only until resume
    bool resetPending = false, explicitContract = false;
    enum class PrSwap : uint8_t { None, WaitingReady, Sent, RetryWait, Done };
    PrSwap prSwap = PrSwap::None;
    bool prSwapRetried = false;
    unsigned alertCount = 0;
    uint64_t alertWindow = 0;

    // The settings this boot changes, as begin() found them (SwitchPDSettings.cpp).
    struct {
        bool valid;
        uint16_t config1, config2, dpAlertEnable, vendor;
        uint8_t alertEnable[4];
    } found = {};

    IONotifier *exitNotifier = nullptr;
    bool leaving = false;  // Shut Down or Restart put the settings back: nothing touches the controller again
    // The USB-PD request (SwitchPD.h).
    switchpd::Nego nego = switchpd::Nego::Idle;
    charge::PdChoice choice = {};
    uint32_t sourcePdos[7] = {};
    unsigned sourcePdoCount = 0, failedRequests = 0;

    bool readStatus(uint16_t &status1, uint16_t &status2);
    void handle(uint16_t alertBits, uint16_t status1, uint16_t status2);
    void apply(const switch_pd_step &step);
    void enter(switch_pd_phase next, const char *why);
    void setSwitches(bool on);
    bool setRails(bool vbus, bool vconn);
    void startSourcing();
    void sendPrSwap();
    void notifyAlt(uint16_t alertBits);
    void notify();
    void stopEvents();
    void systemReset(uint32_t settleMS, const char *why) const;
    static int enableOcp();
    void configureRoleSwaps(bool announce);
    void setVconnSwap(bool accept);
    void recordAsFound();
    void leave();
    static IOReturn powerMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t);
    bool handleFault(uint16_t alertBits, uint16_t status1);
    void negotiate(uint16_t alertBits);
    void choose();
    void verify();
    void publish();
    void withdraw();
    void fail(const char *why);
    void endNegotiation();
    static bool readContract(uint32_t &pdo, uint32_t &rdo);
    static void alertFired(OSObject *owner, IOInterruptEventSource *, int);
    static void windowFired(OSObject *owner, IOTimerEventSource *);
    static void negoFired(OSObject *owner, IOTimerEventSource *);

public:
    IOWorkLoop *getWorkLoop() const override
    {
        return loop;
    }

    bool begin(uint16_t &type, uint16_t &revision, uint16_t &config, bool &turnedOff);
    void watchForExit();
    IOReturn addListener(switchpd::Listener listener);
    void setAllowSource(bool allow);
    void systemSleep(bool entering);
    void describe(char *buffer, size_t size);
};
