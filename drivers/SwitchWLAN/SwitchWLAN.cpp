// SPDX-License-Identifier: BSD-2-Clause-Patent
// Switch OLED Wi-Fi radio: the BCM4356A3 FullMAC chip on Tegra PCIe root port 1, its firmware booted from the
// kernel image, driven over Broadcom's msgbuf rings with the chip's MSI, and a station on top: scans, joins of open
// and WPA2-Personal networks (our own supplicant, SwitchWLANWpa.cpp), the data path to en0 (SwitchWLANNet.cpp),
// and a watchdog that brings the chip back when it resets itself.
//
// Boot-arg switch-no-wlan turns it off (switch_wlan_publish returns at once), to tell a radio that misbehaves from the
// rest of the boot. One worker thread owns the chip: bring-up
// (UPHY calibration, link training and the firmware's start take seconds), then every request from the network
// side (they only queue work here), joins and their key handshakes, the watchdog and recoveries. The work loop
// takes the MSI: completions, firmware events and received frames. Milestone lines:
//   SwitchWLAN: PCIe link up on port 1 (Gen1 x1): 14e4:43ec, BAR0 ... BAR2 ...
//   SwitchWLAN: firmware: wl0: ... version 7.35.252.108 ... / radio up, MAC ... / en0 ready
//   SwitchWLAN: joining '<ssid>' (WPA2) ... / joined '<ssid>' on channel C, -R dBm / link lost (reason N)
// A failed bring-up stage prints "SwitchWLAN: stopped at <stage>: <why>" and leaves the radio idle; the boot goes on.
// Interrupt rules (drivers/ArmGIC/SwitchInterrupts.h): no polled mode. The MSI line that cannot be attached, or
// a chip interrupt latched but never delivered, panics with one line naming the registers and the GIC state. The
// watchdog is not a polling fallback: once a second it looks at what no interrupt reports (the firmware's crash
// flags, a bus reading all ones, the chip's memory decoding turned off by a self-reset, all seen by nxk on Erista).
//
// Sources: MyEcoria/nxk user/wlan (MIT OR Apache-2.0) main.rs (bring-up, `check`, `lost`, `recover`) and wl.rs
// (`init`, `scan`, `join`, `apply`, `add_key`, `disconnect`, `link`); Linux brcmfmac (CTCaer switch-l4t-kernel-4.9)
// fwil_types.h/cfg80211.c for the commands and layouts.
#include <IOKit/IOCommandGate.h>
#include <os/log.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/random.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../SwitchBluetooth/SwitchCoex.h"
#include "SwitchWLANAwdlRadio.h"
#include "SwitchWLANCoex.h"
#include "SwitchWLANCrypto.h"
#include "SwitchWLANLink.h"
#include "SwitchWLANNet.h"
#include "SwitchWLANPrivate.h"
#include "SwitchWLANStation.h"
#include "SwitchWLANTrace.h"
#include "SwitchWLANWpa.h"
#include "../SwitchSuspend/SwitchWake.h"
#include "SwitchWLANAwdlSoft.h"
#include "awdl/awdl_bcm.h"

// firmware.S
extern "C" const uint8_t switch_wlan_fw_image[], switch_wlan_fw_nvram[];
extern "C" const uint64_t switch_wlan_fw_image_size, switch_wlan_fw_nvram_size;

namespace {

// Tegra210 PCIe "msi" interrupt: GIC SPI 99, level high (Linux tegra210.dtsi pcie@1003000; the OLED DTB).
constexpr uint32_t kMsiIntid = 131;
constexpr uint32_t kIoctlTimeoutMs = 2000, kScanTimeoutMs = 15000, kJoinTimeoutMs = 20000, kRecheckMs = 50;
#if DEVELOPMENT || DEBUG
constexpr bool kTxReadBack = true;  // a DNS query's transmit buffer read back once a second (logProbe)
#else
constexpr bool kTxReadBack = false;
#endif
constexpr uint32_t kWatchdogMs = 1000, kRssiRefreshMs = 10000, kTxWaitMs = 1000, kFlowWaitMs = 1000, kStatsMs = 5000;
// r16-idle: the worker's tick and the chip watchdog with it, once the chip is up and no scan runs: what MSIs tell
// (firmware halt, the bus gone) wakes the worker at once, so this only bounds how late a silent self-reset is seen.
// A window with only an idle trickle in it is logged once a minute (kStatsQuietMs, logStats).
constexpr uint32_t kIdleTickMs = 5000;
#if DEVELOPMENT || DEBUG
constexpr uint32_t kStatsQuietMs = 60000, kStatsTrickleFrames = 100;
#endif
constexpr uint32_t kRetryFirstMs = 2000, kRetryMaxMs = 60000;

// Firmware commands (brcmfmac fwil_types.h / nxk wl.rs).
constexpr uint32_t WLC_GET_VERSION = 1, WLC_UP = 2, WLC_DOWN = 3, WLC_GET_RATE = 12, WLC_SET_INFRA = 20, WLC_SCAN = 50;
constexpr uint32_t WLC_DISASSOC = 52, WLC_GET_PM = 85;
constexpr uint32_t WLC_SET_PM = 86;
constexpr uint32_t WLC_GET_RSSI = 127, WLC_GET_BSS_INFO = 136, WLC_GET_VAR = 262, WLC_SET_VAR = 263;

// Firmware events (brcmfmac fweh.h).
enum : uint32_t {
    E_SET_SSID = 0,
    E_JOIN = 1,
    E_AUTH = 3,
    E_DEAUTH = 5,
    E_DEAUTH_IND = 6,
    E_ASSOC = 7,
    E_DISASSOC = 11,
    E_DISASSOC_IND = 12,
    E_LINK = 16,
    E_EAPOL_MSG = 25,
    E_IF = 54,
    E_ESCAN_RESULT = 69,
};

constexpr uint32_t kEventsWanted[] = {E_SET_SSID, E_JOIN,         E_AUTH, E_DEAUTH,    E_DEAUTH_IND, E_ASSOC,
                                      E_DISASSOC, E_DISASSOC_IND, E_LINK, E_EAPOL_MSG, E_IF,         E_ESCAN_RESULT};
// The firmware's own AWDL (drivers/SwitchWLAN/awdl/awdl_bcm.h): ACTION_FRAME_RX (75, the AWDL interface's received
// AWDL action frames, as the brcmfmac AWDL series reads them), AW, role, generic (96-98), then WHD's 111-120 (frames,
// status, AW start/end/extension, peer cache). Asked for only while AWDL is open.
constexpr uint32_t E_ACTION_FRAME_RX = 75;

bool isAwdlEvent(uint32_t type)
{
    return (type >= 96 && type <= 98) || (type >= 111 && type <= 120) || type == BCM_E_ACTION_FRAME_COMPLETE ||
           type == BCM_E_ACTION_FRAME_OFF_CHAN_COMPLETE;
}

constexpr uint32_t kStatusPartial = 8, kLinkUp = 1;
constexpr uint32_t kWpa2AuthPsk = 0x80, kAesEnabled = 4, kReasonLeaving = 3;
// brcmf_wsec_key_le (164 bytes): index, len, data[32], ..., algo @112, flags @116, iv_initialized @132,
// rxiv {hi u32, lo u16} @140, ea @156.
constexpr size_t kKeySize = 164, kKeyAlgo = 112, kKeyFlags = 116, kKeyIvInit = 132, kKeyRxIv = 140, kKeyEa = 156;
constexpr uint32_t kAlgoAesCcm = 4, kPrimaryKey = 1u << 1;
constexpr uint16_t kEthPLinkCtl = 0x886C;

// Printable SSID for a log line.
void printable(const uint8_t *in, size_t length, char *out)
{
    size_t i = 0;
    for (; i < length && i < 32; ++i)
        out[i] = (in[i] >= 0x20 && in[i] < 0x7F) ? static_cast<char>(in[i]) : '?';
    out[i] = 0;
}

uint32_t channelToMhz(uint32_t channel)
{
    if (channel == 14)
        return 2484;
    return channel <= 14 ? 2407 + 5 * channel : 5000 + 5 * channel;
}

void randomBytes(uint8_t *out, size_t length)
{
    read_random(out, static_cast<u_int>(length));
}

int8_t clampDbm(int32_t dbm)
{
    if (dbm < -128)
        return -128;
    return static_cast<int8_t>(dbm > 0 ? 0 : dbm);
}

const char *linkDownCause(uint32_t type)
{
    switch (type) {
    case E_DEAUTH_IND:
        return "deauthenticated by the access point";
    case E_DISASSOC_IND:
        return "disassociated by the access point";
    case E_DEAUTH:
        return "deauthentication sent";
    case E_DISASSOC:
        return "disassociation sent";
    default:
        return "link down";
    }
}

// One brcmf_bss_info_le (nxk parse_bss), as escan results and WLC_GET_BSS_INFO give them: its own length, then the
// BSSID, beacon interval, capabilities, SSID, channel (the control channel, else the chanspec's), the width its
// chanspec (in the firmware's `iotype` layout) says, RSSI and the information elements. False if the record does not
// fit in `room` bytes.
bool parseBss(const uint8_t *b, size_t room, swlan_bss &s, int iotype)
{
    if (room < 124)
        return false;
    const uint32_t length = swlan::le32(b + 4);
    if (length < 124 || length > room)
        return false;
    memset(&s, 0, sizeof(s));
    memcpy(s.bssid, b + 8, 6);
    s.beacon_int = swlan::le16(b + 14);
    s.caps = swlan::le16(b + 16);
    s.ssid_len = b[18] > 32 ? 32 : b[18];
    memcpy(s.ssid, b + 19, s.ssid_len);
    const uint32_t channel = b[88] ? b[88] : (swlan::le16(b + 72) & 0xFF);
    s.freq_mhz = channelToMhz(channel);
    swlan_chanspec cs;
    if (swlan_chanspec_decode(swlan::le16(b + 72), iotype, &cs)) {
        s.bw_mhz = cs.bw_mhz;
        s.center = cs.center;
    }
    const int16_t rssi = static_cast<int16_t>(swlan::le16(b + 78));
    s.signal_dbm = clampDbm(rssi);
    const uint32_t ieOffset = swlan::le16(b + 116);
    const uint32_t ieLength = swlan::le32(b + 120);
    if (ieOffset + ieLength <= length) {
        s.ie_len = static_cast<uint16_t>(ieLength < sizeof(s.ie) ? ieLength : sizeof(s.ie));
        memcpy(s.ie, b + ieOffset, s.ie_len);
    }
    return true;
}

uint64_t deadlineIn(uint32_t ms)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    return deadline;
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class SwitchWLAN : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
OSDeclareDefaultStructors(SwitchWLAN)

    public : bool start(IOService *provider) override;

    // System sleep (switch_wlan_suspend / switch_wlan_resume), SwitchSuspend's thread.
    void systemSleep();
    void systemWake();

    // The station interface (SwitchWLANStation.h), any thread: each takes the gate briefly.
    void setEvents(const swlan_events *events);
    int requestScan(const uint8_t *ssid, size_t length);
    int bssCount();
    int bssGet(int index, swlan_bss *out);
    int currentBss(swlan_bss *out);
    int requestJoin(const uint8_t *ssid, size_t ssidLength, const uint8_t *bssid, const uint8_t *key, size_t keyLength,
                    bool keyIsPmk);
    void requestLeave();
    int requestPower(bool on);
    int transmit(const uint8_t *frame, size_t length);
    unsigned txRoom();
    unsigned transmitFrames(swlan_tx_fill_fn fill, void *context, unsigned max);
    void status(swlan_status *out);
    void counters(char *text, size_t size);

    void setMac(const uint8_t mac[6])
    {
        memcpy(mac_, mac, 6);
    }

    void btState(uint32_t flags, bool power);
    void btLinks(unsigned links);
    void multicast(const uint8_t (*groups)[6], unsigned count, bool all);

    // The firmware's own AWDL for SwitchWLANAwdl.cpp (SwitchWLANAwdlRadio.h §6). Open/close and the iovars wait for
    // the firmware: thread context only, never the work loop. The callbacks are only stored here; the work loop calls
    // them with the gate held (the stack never calls back into a gated call while holding its own lock).
    int awdlOpen(const swlan_awdl_caps **caps);
    int awdlQuery(const swlan_awdl_caps **caps);
    void awdlClose();
    int awdlVar(bool set, uint8_t ifidx, const char *name, const void *in, size_t inLength, void *out, size_t outLength,
                size_t *returned);
    int awdlTransmit(const uint8_t *eth, size_t length);
    int awdlIoctlSet(uint32_t cmd, const void *buf, size_t length);  // the soft backend's actframe and monitor mode

    int awdlD11n() const
    {
        return iotype_ == SWLAN_IOTYPE_D11N ? 1 : 0;
    }

    void awdlCallbacks(swlan_fw_event_fn events, void *eventsContext, swlan_eth_rx_fn rx, void *rxContext,
                       swlan_evt_fn loss, void *lossContext);

private:
    struct JoinParams {
        uint8_t ssid[32];
        size_t ssidLength;
        uint8_t bssid[6];
        bool hasBssid, hasKey, keyIsPmk;
        uint8_t key[64];
        size_t keyLength;
    };

    struct FwEvent {
        uint32_t type, status, reason, flags;
        uint8_t addr[6];
    };

    struct EapolFrame {
        uint16_t length;
        uint8_t frame[512];
    };

    IOWorkLoop *loop_ = nullptr;
    IOCommandGate *gate_ = nullptr;
    IOInterruptEventSource *msi_ = nullptr;
    swlan::TegraPcie pcie_;
    swlan::TegraPcie::Endpoint ep_ = {};
    swlan::Chip chip_;
    swlan::Msgbuf mb_;
    // Chip state: up_ while the rings are served; gone_/halted_ as the MSI path found it.
    bool up_ = false, gone_ = false, halted_ = false, netStarted_ = false;
    uint8_t mac_[6] = {};
    uint64_t interrupts_ = 0;
    char why_[512] = {};
    const swlan_events *events_ = nullptr;

    // Under the gate: work for the worker, requests, station state, scan results, firmware events and EAPOL frames
    // from the work loop.
    bool work_ = false;
    bool scanRequested_ = false, joinRequested_ = false, leaveRequested_ = false, powerRequested_ = false;
    bool powerOn_ = true;
    uint8_t scanSsid_[32] = {};
    size_t scanSsidLength_ = 0;
    JoinParams join_ = {}, last_ = {};
    bool haveLast_ = false;
    swlan_status status_ = {};
    bool scanning_ = false, scanDone_ = false;
    uint32_t scanStatus_ = 0;
    // The escan's sync id, which the firmware copies into its results, and that of the last scan given up (abortScan).
    uint16_t scanSync_ = 0x5357, abortedSync_ = 0;
    // The scan table, the joined access point and the EAPOL queue live outside the object (an IOService is
    // kalloc_type'd, 16 KiB at most). A busy place has more than 32 networks in range (wifid listed 37 on r3v).
    static constexpr unsigned kBssMax = 64, kEapolMax = 8;
    swlan_bss *bss_ = nullptr, *current_ = nullptr;
    uint64_t *bssSeenNs_ = nullptr;
    unsigned bssCount_ = 0;
    FwEvent fwEvents_[16] = {};
    unsigned fwHead_ = 0, fwCount_ = 0;
    EapolFrame *eapol_ = nullptr;
    unsigned eapolHead_ = 0, eapolCount_ = 0;
    uint32_t eapolDropped_ = 0, rxToStack_ = 0, losses_ = 0, recoveries_ = 0;
    uint32_t rxGroup_ = 0, rxUnicast_ = 0;  // frames to en0 by destination: group (multicast, broadcast) or ours
    uint64_t rxBytes_ = 0, txBytes_ = 0;    // bytes to and from en0, for the stats line's throughput
    int iotype_ = SWLAN_IOTYPE_D11AC;       // the firmware's chanspec layout (WLC_GET_VERSION)
    swlan_peer peer_ = {};                  // the joined access point's HT/VHT/HE, from its full beacon elements
    // Coexistence ("Coexistence" section): what wifid says Bluetooth is doing (SWLAN_BT_*), and its power.
    uint32_t btFlags_ = 0;
    bool btPower_ = true, btChanged_ = false;
    unsigned btLinks_ = 0;  // SwitchBluetooth's links (switch_wlan_bt_links)
    // en0's multicast filter (switch_wlan_multicast); mcastChanged_: not yet given to the firmware.
    uint8_t mcast_[SWLAN_MCAST_MAX][6] = {};
    unsigned mcastCount_ = 0;
    bool mcastAll_ = false, mcastChanged_ = false;
    void multicastApply();

    // AWDL: the firmware's cap list has "awdl"; open (event mask on, frames and events routed); the AWDL
    // interface's firmware index from its WLC_E_IF (-1 until then).
    bool capAwdl_ = false, capRead_ = false, awdlOpen_ = false;
    int awdlIfidx_ = -1, awdlBssidx_ = -1;  // from its WLC_E_IF add (wl_event_data_if_t ifidx, bssidx)
    bool awdlStrayFrameSaid_ = false;
    uint32_t awdlRxDropped_ = 0, awdlEventsIn_ = 0;
    swlan_fw_event_fn awdlEventFn_ = nullptr;
    void *awdlEventCtx_ = nullptr;
    swlan_eth_rx_fn awdlRxFn_ = nullptr;
    void *awdlRxCtx_ = nullptr;
    swlan_evt_fn awdlLossFn_ = nullptr;
    void *awdlLossCtx_ = nullptr;
    bool setEventMask(bool awdl);
    void awdlEvent(const swlan::Event &event);
    // Once a boot, after the first bring-up: GET of every AWDL variable either firmware generation uses, one os_log
    // line each (status, bytes), so one boot settles which AWDL API this firmware's table has (gAwdlProbe).
    void awdlProbe();
    void monitorProbe();

    // Worker only.
    swlan::wpa::Supplicant sup_;
    bool secured_ = false;
    uint64_t nextRssiNs_ = 0, scanEndNs_ = 0, nextStatsNs_ = 0;
    uint64_t statsSinceNs_ = 0;  // the start of the window logStats is counting (lastCounts_)

    // The data path's counters at the last stats line (logStats).
    struct Counts {
        uint32_t rx, tx, done, refused, failed, rxGroup, rxUnicast;
        uint64_t msis, rxBytes, txBytes;
        swlan_net_counters net;
        swlan::DataCounters data;
        swlan_trace_counts trace;
    } lastCounts_ = {};

    void snapshot(Counts &c);
    void logStats();
    void logProbe(const Counts &now, const Counts &was, uint32_t windowMs);
    // Release kernels: the link rates logStats keeps current in debug ones, without the line.
    void refreshRates();
    uint32_t ratesFrames_ = 0;
    // The firmware's "counters" block now and at the last stats line (worker only; too large for its stack).
    uint8_t fwCounters_[1536] = {}, fwCountersNow_[1536] = {};
    size_t fwCountersLength_ = 0;
    bool fwCountersRefused_ = false, pmReadRefused_ = false, sleepRetReadRefused_ = false;
    // Under the gate: the bytes of the transmit buffer read back next (one DNS query a second at most).
    uint8_t txWritten_[96] = {};
    uint64_t nextReadBackNs_ = 0;
    // The access point's counters (sta_info) and the firmware's time in power save (pm_dur, ms) at the last stats
    // line; an iovar the firmware refused once is not asked again.
    swlan_sta staLast_ = {};
    uint32_t pmDurLast_ = 0;
    bool staRefused_ = false, pmDurRefused_ = false, pmDurHave_ = false, btcStatusRefused_ = false;
    // The firmware's A-MPDU counters (its "dump ampdu" text) at the last stats line, for MPDUs per A-MPDU each way;
    // a firmware without that dump, or one whose text has no counters, gets one line and is not asked again.
    uint32_t ampdu_[4] = {};  // txampdu, txmpdu, rxampdu, rxmpdu
    bool ampduHave_ = false, ampduRefused_ = false;
    void ampduWindow(char *out, size_t size);
    bool varInt(const char *name, uint32_t &value);
    bool varIntWith(const char *name, uint32_t param, uint32_t &value);
    void logLink();
    // The power-save mode the station runs (coexApply) and whether the firmware took HOS's sleep return this join.
    uint32_t pm_ = 0;
    bool sleepRetSet_ = false, sleepRetRefused_ = false;
    // The coexistence mode (coexMode): the firmware's own at bring-up (its idle mode), the one we last set, refusals.
    uint32_t btcIdleMode_ = SWLAN_BTC_HYBRID, btcMode_ = 0;
    bool btcModeRefused_ = false;
    // The dynamic coexistence profile the firmware held at bring-up (btc_dynctl, the NVRAM's), and the flags of the
    // one it holds now.
    uint8_t btcProfile_[50] = {};
    bool btcProfileHave_ = false, btcProfileRefused_ = false;
    int btcProfileFlags_ = -1;
    void coexProfile(unsigned links);
    bool readSta(swlan_sta &out);
    void coexBringUp();
    void coexJoined();
    void coexApply();
    void coexConfigure();
    void coexMode();

    template <typename F> void locked(F f)
    {
        gate_->runAction(
            [](OSObject *, void *argument, void *, void *, void *) -> IOReturn {
                (*static_cast<F *>(argument))();
                return kIOReturnSuccess;
            },
            &f);
    }

    void kick()
    {
        work_ = true;
        gate_->commandWakeup(&work_);
    }  // gate held

    void waitWork(uint32_t ms);

    static void thread(void *argument, wait_result_t);
    void run();
    bool bringUp();
    bool bootChip(const char **stage);
    void stop(const char *stage);
    void attachInterrupt();
    static void msiFired(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void onEvent(void *context, const swlan::Event &event);
    static void onFrame(void *context, const uint8_t *frame, size_t length, uint8_t ifidx, uint8_t kind);
    void escanResult(const swlan::Event &event);
    void keepBss(const swlan_bss &bss, uint64_t now);
    void pushEapol(const uint8_t *frame, size_t length);

    struct Call {
        uint32_t cmd;
        const uint8_t *data;
        size_t length, outLength;
        uint8_t *out;
        size_t *outReturned;
        bool ok;
        uint8_t ifidx;     // the firmware interface (msgbuf ioctl ifidx)
        int16_t fwStatus;  // the firmware's BCME code when it answered with an error, 0 otherwise
    };

    static IOReturn ioctlGated(OSObject *owner, void *argument, void *, void *, void *);
    bool ioctl(uint32_t cmd, const uint8_t *data, size_t length, size_t outLength, uint8_t *out, size_t *outReturned);
    // The same on firmware interface `ifidx`; *fwStatus gets the firmware's BCME code when it refused (0 when the
    // request did not reach it or succeeded).
    bool ioctlOn(uint8_t ifidx, uint32_t cmd, const uint8_t *data, size_t length, size_t outLength, uint8_t *out,
                 size_t *outReturned, int16_t *fwStatus);
    bool getVar(const char *name, uint8_t *out, size_t outLength, size_t *returned);
    bool getVarWith(const char *name, const void *param, size_t paramLength, uint8_t *out, size_t outLength,
                    size_t *returned);
    bool setVar(const char *name, const void *value, size_t length);
    bool cmdInt(uint32_t cmd, uint32_t value);
    bool radioInit();

    bool startScan(const uint8_t *ssid, size_t ssidLength);
    void abortScan();
    void finishScan();
    bool scanAndWait(const uint8_t *ssid, size_t ssidLength);
    bool readJoinedBss(swlan_bss &out);
    void join(const JoinParams &params);
    int joinOnce(const JoinParams &params, char *ssidText);
    bool ensureFlow(const uint8_t destination[6]);
    uint32_t sendFrame(const uint8_t *frame, size_t length);
    bool waitSent(uint32_t id);
    bool addKey(uint32_t index, const uint8_t key[16], const uint8_t *peer, const uint8_t *rsc);
    bool handleEapol(const uint8_t *frame, size_t length);
    void leave();
    void linkDown(const FwEvent &event);
    bool popEvent(FwEvent &event);
    bool popEapol(EapolFrame &frame);
    void watchdog();
    void lost(const char *why);
    bool recover(bool afterSleep = false);
    void parkForSleep();

    // Under the gate: the worker's part in system sleep (kAwake; kSleepAsked by systemSleep; kParked while the worker
    // waits for the wake; kWakeAsked by systemWake) and whether the worker loop runs at all.
    enum : uint8_t {
        kAwake,
        kSleepAsked,
        kParked,
        kWakeAsked
    };

    uint8_t sleep_ = kAwake;
    bool workerRunning_ = false;
    bool sleptRecover_ = false;  // worker thread: the next recovery follows SC7
    void setState(uint32_t state);
};

OSDefineMetaClassAndStructors(SwitchWLAN, IOService)

    namespace
{
    SwitchWLAN *gDriver;
    // The infra channel (MHz) while associated, 0 otherwise: read lock-free by SwitchWLANAwdl.cpp, whose schedule puts
    // AWDL's free slots on the access point's channel (no mchan on this firmware: AWDL and the station share the
    // one radio by time).
    uint32_t gInfraFreq;
    // What the firmware's iovar table answered to the AWDL probe (awdlProbe, once a boot).
    bcm_awdl_probe gAwdlProbe;
}

// A short AWDL suffix for the stats line, written by SwitchWLANAwdl.cpp (empty when AWDL is off); weak so the driver
// links without the AWDL unit.
// NOLINTNEXTLINE(misc-const-correctness,readability-non-const-parameter): the strong definition writes buf
extern "C" __attribute__((weak)) int switch_wlan_awdl_stat(char *buf, size_t size)
{
    (void)buf;
    (void)size;
    return 0;
}

// SwitchBluetooth's coexistence snapshot (SwitchCoex.h); weak so the driver links without the Bluetooth unit.
// NOLINTNEXTLINE(misc-const-correctness): the strong definition (SwitchBluetooth.cpp) fills state
extern "C" __attribute__((weak)) bool switch_bluetooth_coex(struct switch_coex_bt *state, char *links, size_t size)
{
    (void)state;
    if (size)
        links[0] = 0;
    return false;
}

bool SwitchWLAN::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    bss_ = static_cast<swlan_bss *>(IOMallocZero(kBssMax * sizeof(swlan_bss)));
    current_ = static_cast<swlan_bss *>(IOMallocZero(sizeof(swlan_bss)));
    bssSeenNs_ = static_cast<uint64_t *>(IOMallocZero(kBssMax * sizeof(uint64_t)));
    eapol_ = static_cast<EapolFrame *>(IOMallocZero(kEapolMax * sizeof(EapolFrame)));
    if (!bss_ || !current_ || !bssSeenNs_ || !eapol_) {
        IOLog("SwitchWLAN: no memory for the scan table\n");
        return false;
    }
    loop_ = IOWorkLoop::workLoop();
    gate_ = loop_ ? IOCommandGate::commandGate(this) : nullptr;
    if (!gate_ || loop_->addEventSource(gate_) != kIOReturnSuccess) {
        IOLog("SwitchWLAN: work loop setup failed\n");
        return false;
    }
    status_.state = SWLAN_STA_OFF;
    status_.power = 1;
    memcpy(status_.mac, mac_, 6);
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(&SwitchWLAN::thread, this, &thread) != KERN_SUCCESS) {
        IOLog("SwitchWLAN: worker thread failed to start\n");
        return false;
    }
    thread_deallocate(thread);
    return true;
}

void SwitchWLAN::thread(void *argument, wait_result_t)
{
    static_cast<SwitchWLAN *>(argument)->run();
    // Returning ends the thread (Call_continuation branches to thread_terminate).
}

void SwitchWLAN::stop(const char *stage)
{
    IOLog("SwitchWLAN: stopped at %s: %s\n", stage, why_);
}

void SwitchWLAN::waitWork(uint32_t ms)
{
    locked([&] {
        const uint64_t deadline = deadlineIn(ms);
        while (!work_ && gate_->commandSleep(&work_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
        }
        work_ = false;
    });
}

void SwitchWLAN::setState(uint32_t state)
{
    locked([&] {
        status_.state = state;
    });
}

// ---- the MSI and what the work loop takes from the firmware ------------------------------------------------------

// The chip's MSI, level-high SPI 99, masked at the GIC while this runs. AFI vector latch cleared first, then the
// chip's mailbox (Msgbuf::service drops the chip's mask meanwhile, so anything new makes a fresh MSI).
void SwitchWLAN::msiFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = OSDynamicCast(SwitchWLAN, owner);
    if (!self)
        return;
    self->pcie_.msiTake();
    ++self->interrupts_;
    if (!self->up_ || self->gone_ || self->halted_)
        return;
    bool halted = false;
    const uint32_t status = self->mb_.service(&halted);
    if (status == 0xFFFFFFFF) {
        self->gone_ = true;
        self->kick();
    } else if (halted) {
        self->halted_ = true;
        self->kick();
    }
    // The interrupt's frames to the stack in one go, and en0's queue started again once completions freed a batch of
    // transmit buffers it ran out of.
    if (const swlan_events *events = self->events_) {
        if (events->rx_flush)
            events->rx_flush();
        if (self->mb_.txResume() && events->tx_ready)
            events->tx_ready();
    }
    self->gate_->commandWakeup(&self->mb_);
}

void SwitchWLAN::attachInterrupt()
{
    if (msi_)
        return;  // one owner per INTID: kept across chip recoveries
    IOService *nub = switch_interrupt_nub(kMsiIntid, false);
    msi_ = nub ? IOInterruptEventSource::interruptEventSource(this, &SwitchWLAN::msiFired, nub, 0) : nullptr;
    if (!msi_ || loop_->addEventSource(msi_) != kIOReturnSuccess) {
        char gic[160];
        switch_interrupt_describe(kMsiIntid, gic, sizeof(gic));
        panic("SwitchWLAN: cannot attach the PCIe MSI interrupt (INTID %u, nub %p): %s", kMsiIntid, nub, gic);
    }
    msi_->enable();
}

// Work loop. Scan results into the table; the events a join or a joined station follows, and EAPOL frames, to the
// worker.
void SwitchWLAN::onEvent(void *context, const swlan::Event &event)
{
    auto *self = static_cast<SwitchWLAN *>(context);
    // WLC_E_IF about any interface but the station's (in the header or in wl_event_data_if_t's ifidx/bssidx)
    const bool otherIf =
        event.type == E_IF && ((event.ifidx != 0u) || (event.bsscfgidx != 0u) ||
                               (event.length >= 5 && ((event.data[0] != 0u) || (event.data[3] != 0u))));
    // received action frames on the AWDL interface (brcmfmac finds the interface by the header's bsscfgidx, we take
    // either field), or any whose body is an AWDL action frame (en0 uses no action frames): the first of those not
    // on the AWDL interface's indices names the fields this firmware fills, once
    bool awdlFrame = false;
    if (event.type == E_ACTION_FRAME_RX && self->awdlOpen_) {
        const bool onAwdl = ((event.ifidx != 0u) && event.ifidx == self->awdlIfidx_) ||
                            ((event.bsscfgidx != 0u) && event.bsscfgidx == self->awdlBssidx_);
        bcm_rx_frame meta;
        const uint8_t *body = nullptr;
        size_t n = 0;
        const bool isAwdl = bcm_awdl_rx_af(event.data, event.length, &meta, &body, &n) == 0;
        awdlFrame = onAwdl || isAwdl;
        if (isAwdl && !onAwdl && !self->awdlStrayFrameSaid_) {
            self->awdlStrayFrameSaid_ = true;
            IOLog("SwitchWLAN: AWDL: AWDL action frames arrive with ifidx %u bsscfg %u (the AWDL interface: ifidx %d "
                  "bsscfg %d): taken by their content\n",
                  event.ifidx, event.bsscfgidx, self->awdlIfidx_, self->awdlBssidx_);
        }
    }
    if (self->awdlOpen_ && (isAwdlEvent(event.type) || otherIf || awdlFrame)) {
        self->awdlEvent(event);
        return;
    }
    switch (event.type) {
    case E_ESCAN_RESULT:
        self->escanResult(event);
        break;
    case E_EAPOL_MSG: {
        // The EAPOL PDU alone: as the Ethernet frame the supplicant takes (sender, then the PAE ethertype).
        uint8_t frame[512] = {};
        const size_t n = event.length < sizeof(frame) - 14 ? event.length : sizeof(frame) - 14;
        memcpy(frame, self->mac_, 6);
        memcpy(frame + 6, event.addr, 6);
        frame[12] = swlan::wpa::kEthPae >> 8;
        frame[13] = swlan::wpa::kEthPae & 0xFF;
        memcpy(frame + 14, event.data, n);
        self->pushEapol(frame, 14 + n);
        break;
    }
    case E_SET_SSID:
    case E_JOIN:
    case E_AUTH:
    case E_DEAUTH:
    case E_DEAUTH_IND:
    case E_ASSOC:
    case E_DISASSOC:
    case E_DISASSOC_IND:
    case E_LINK: {
        FwEvent &e = self->fwEvents_[(self->fwHead_ + self->fwCount_) % 16];
        if (self->fwCount_ == 16) {
            self->fwHead_ = (self->fwHead_ + 1) % 16;  // oldest dropped
        } else {
            ++self->fwCount_;
        }
        e = {event.type, event.status, event.reason, event.flags, {}};
        memcpy(e.addr, event.addr, 6);
        self->kick();
        break;
    }
    default:
        break;
    }
}

void SwitchWLAN::pushEapol(const uint8_t *frame, size_t length)
{
    if (length > sizeof(eapol_[0].frame))
        return;
    if (eapolCount_ == kEapolMax) {
        ++eapolDropped_;
        return;
    }
    EapolFrame &f = eapol_[(eapolHead_ + eapolCount_++) % kEapolMax];
    f.length = static_cast<uint16_t>(length);
    memcpy(f.frame, frame, length);
    kick();
}

// Work loop: a received frame. EAPOL to the supplicant, the rest to en0 once joined (a station has no carrier
// before that).
void SwitchWLAN::onFrame(void *context, const uint8_t *frame, size_t length, uint8_t ifidx, uint8_t kind)
{
    auto *self = static_cast<SwitchWLAN *>(context);
    if (kind == BCM_MSGBUF_RX_FRAME_802_11) {  // monitor mode's: only the soft AWDL backend asks for it
        if (self->awdlOpen_) {
            switch_wlan_soft_frame(frame, length);
        } else {
            ++self->awdlRxDropped_;
        }
        return;
    }
    if (ifidx) {  // another firmware interface: AWDL's, while it is open
        if (self->awdlOpen_ && self->awdlRxFn_ && static_cast<int>(ifidx) == self->awdlIfidx_) {
            self->awdlRxFn_(self->awdlRxCtx_, frame, length);
        } else {
            ++self->awdlRxDropped_;
        }
        return;
    }
    if (swlan::wpa::isEapol(frame, length)) {
        self->pushEapol(frame, length);
        return;
    }
    if (((static_cast<unsigned>(frame[12]) << 8) | frame[13]) == kEthPLinkCtl)
        return;  // firmware event frames take the event path
    if (self->status_.state == SWLAN_STA_UP && self->events_ && self->events_->rx) {
        ++self->rxToStack_;
        ++(frame[0] & 1 ? self->rxGroup_ : self->rxUnicast_);
        self->rxBytes_ += length;
        self->events_->rx(frame, length);
    }
}

// brcmf_escan_result_le: buflen, version, sync id, BSS count, then brcmf_bss_info_le records. A late record of a
// scan given up (abortScan, its final "aborted" one above all) would end the current scan early and empty: dropped
// by its sync id.
void SwitchWLAN::escanResult(const swlan::Event &event)
{
    const uint8_t *data = event.data;
    const uint64_t now = swlan::nowNs();
    if (event.length >= 10 && abortedSync_ && abortedSync_ != scanSync_ && swlan::le16(data + 8) == abortedSync_)
        return;
    if (event.length >= 12) {
        const unsigned count = swlan::le16(data + 10);
        uint32_t at = 12;
        for (unsigned i = 0; i < count && at < event.length; ++i) {
            swlan_bss s;
            if (!parseBss(data + at, event.length - at, s, iotype_))
                break;
            keepBss(s, now);
            at += swlan::le32(data + at + 4);
        }
    }
    if (event.status != kStatusPartial) {
        scanStatus_ = event.status;
        scanDone_ = true;
        kick();
    }
}

// Into the scan table (gate held). A network seen again keeps its strongest record, or a fresh one after a second;
// a new one takes a free slot, or the weakest network's once the table is full (when it is stronger).
void SwitchWLAN::keepBss(const swlan_bss &s, uint64_t now)
{
    unsigned k = 0;
    while (k < bssCount_ && memcmp(bss_[k].bssid, s.bssid, 6))
        ++k;
    if (k == bssCount_ && bssCount_ < kBssMax) {
        ++bssCount_;
    } else if (k == bssCount_) {
        k = 0;
        for (unsigned i = 1; i < kBssMax; ++i) {
            if (bss_[i].signal_dbm < bss_[k].signal_dbm)
                k = i;
        }
        if (bss_[k].signal_dbm >= s.signal_dbm)
            return;
    } else if (bss_[k].signal_dbm > s.signal_dbm && now - bssSeenNs_[k] <= 1000000000ULL) {
        return;
    }
    bss_[k] = s;
    bssSeenNs_[k] = now;
}

// ---- firmware commands -------------------------------------------------------------------------------------------

// One ioctl under the gate: posted, then the gate released while the MSI path delivers the answer. A timeout
// with the chip still signalling (or the AFI's MSI latch set) after a short grace period means the interrupt
// never reached the CPU: panic. A timeout with nothing pending is the firmware's silence: reported, not fatal.
IOReturn SwitchWLAN::ioctlGated(OSObject *owner, void *argument, void *, void *, void *)
{
    auto *self = static_cast<SwitchWLAN *>(owner);
    auto *call = static_cast<Call *>(argument);
    call->ok = false;
    if (!self->up_ || self->gone_ || self->halted_) {
        const char *state = self->halted_ ? "firmware halted" : "not up";
        if (self->gone_)
            state = "gone from the bus";
        snprintf(self->why_, sizeof(self->why_), "chip %s", state);
        return kIOReturnNotReady;
    }
    if (!self->mb_.ioctlSubmit(call->cmd, call->ifidx, call->data, call->length, call->outLength, self->why_,
                               sizeof(self->why_)))
        return kIOReturnNoResources;
    uint64_t deadline = deadlineIn(kIoctlTimeoutMs);
    while (!self->mb_.replyReady() && !self->gone_ && !self->halted_) {
        if (self->gate_->commandSleep(&self->mb_, deadline, THREAD_UNINT) == THREAD_TIMED_OUT)
            break;
    }
    if (!self->mb_.replyReady()) {
        uint32_t mailboxInt = 0;
        uint32_t mailboxMask = 0;
        if (!self->gone_ && !self->halted_ &&
            (self->mb_.chipSignalling(&mailboxInt, &mailboxMask) || self->pcie_.msiPending())) {
            deadline = deadlineIn(kRecheckMs);
            while (!self->mb_.replyReady() &&
                   self->gate_->commandSleep(&self->mb_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
            }
            if (!self->mb_.replyReady() &&
                (self->mb_.chipSignalling(&mailboxInt, &mailboxMask) || self->pcie_.msiPending())) {
                char gic[160];
                char state[200];
                switch_interrupt_describe(kMsiIntid, gic, sizeof(gic));
                self->pcie_.describe(state, sizeof(state));
                panic(
                    "SwitchWLAN: chip interrupt not delivered: mailbox int %08x mask %08x, %u MSIs so far; %s; GIC %s",
                    mailboxInt, mailboxMask, static_cast<unsigned>(self->interrupts_), state, gic);
            }
        }
        if (!self->mb_.replyReady()) {
            char console[200];
            self->chip_.consoleTail(console, sizeof(console));
            snprintf(self->why_, sizeof(self->why_),
                     "no answer to ioctl %u in %u ms (mailbox int %08x mask %08x, %u "
                     "MSIs); console: %s",
                     call->cmd, kIoctlTimeoutMs, mailboxInt, mailboxMask, static_cast<unsigned>(self->interrupts_),
                     console);
            return kIOReturnTimeout;
        }
    }
    if (self->mb_.replyStatus()) {
        call->fwStatus = self->mb_.replyStatus();
        snprintf(self->why_, sizeof(self->why_), "ioctl %u: firmware error %d", call->cmd, self->mb_.replyStatus());
        return kIOReturnError;
    }
    if (call->out) {
        const size_t n = self->mb_.replyLength() < call->outLength ? self->mb_.replyLength() : call->outLength;
        memcpy(call->out, self->mb_.replyData(), n);
        if (call->outReturned)
            *call->outReturned = n;
    }
    call->ok = true;
    return kIOReturnSuccess;
}

bool SwitchWLAN::ioctl(uint32_t cmd, const uint8_t *data, size_t length, size_t outLength, uint8_t *out,
                       size_t *outReturned)
{
    return ioctlOn(0, cmd, data, length, outLength, out, outReturned, nullptr);
}

bool SwitchWLAN::ioctlOn(uint8_t ifidx, uint32_t cmd, const uint8_t *data, size_t length, size_t outLength,
                         uint8_t *out, size_t *outReturned, int16_t *fwStatus)
{
    Call call = {cmd, data, length, outLength, out, outReturned, false, ifidx, 0};
    gate_->runAction(&SwitchWLAN::ioctlGated, &call);
    if (fwStatus)
        *fwStatus = call.ok ? 0 : call.fwStatus;
    return call.ok;
}

// brcmf_fil_iovar_data_get / _set: the variable's name, NUL, then the value.
bool SwitchWLAN::getVar(const char *name, uint8_t *out, size_t outLength, size_t *returned)
{
    return ioctl(WLC_GET_VAR, reinterpret_cast<const uint8_t *>(name), strlen(name) + 1, outLength, out, returned);
}

// The same with a parameter after the name (brcmf_fil_bsscfg_data_get's buffer: sta_info takes the station's address).
bool SwitchWLAN::getVarWith(const char *name, const void *param, size_t paramLength, uint8_t *out, size_t outLength,
                            size_t *returned)
{
    uint8_t request[64];
    const size_t nameLength = strlen(name) + 1;
    if (nameLength + paramLength > sizeof(request))
        return false;
    memcpy(request, name, nameLength);
    memcpy(request + nameLength, param, paramLength);
    return ioctl(WLC_GET_VAR, request, nameLength + paramLength, outLength, out, returned);
}

bool SwitchWLAN::setVar(const char *name, const void *value, size_t length)
{
    uint8_t request[256];
    const size_t nameLength = strlen(name) + 1;
    if (nameLength + length > sizeof(request))
        return false;
    memcpy(request, name, nameLength);
    if (length)
        memcpy(request + nameLength, value, length);
    return ioctl(WLC_SET_VAR, request, nameLength + length, 0, nullptr, nullptr);
}

bool SwitchWLAN::cmdInt(uint32_t cmd, uint32_t value)
{
    uint8_t le[4];
    swlan::put32(le, value);
    return ioctl(cmd, le, sizeof(le), 0, nullptr, nullptr);
}

// nxk wl.rs `init` (brcmf_c_preinit_dcmds, brcmf_config_dongle): our address, the events handled, station mode,
// no power saving, no firmware roaming (it would start a key handshake with another access point behind the
// supplicant's back; wifid decides on roams), en0's multicast groups (multicastApply), brcmf's join preference
// (brcmf_c_set_joinpref_default: by RSSI, 5 GHz counted 8 dB better; the firmware picks the access point when wifid
// names none, and without it r4a sat on 2.4 GHz at -45 dBm next to the same network's 5 GHz at -44, sharing the air
// with Bluetooth), interface up; then the regulatory domain.
bool SwitchWLAN::radioInit()
{
    const uint8_t on[4] = {1, 0, 0, 0};
    // brcmf_join_pref_params {type, length, RSSI gain, band}: RSSI_DELTA (4) +8 dB on WLC_BAND_5G (1), then RSSI (1)
    const uint8_t joinPref[8] = {4, 2, 8, 1, 1, 2, 0, 0};
    bool awdl = false;
    locked([&] {
        awdl = awdlOpen_;
    });  // after a recovery an open AWDL keeps its events
    if (!setVar("cur_etheraddr", mac_, 6) || !setEventMask(awdl) || !cmdInt(WLC_SET_INFRA, 1) || !cmdInt(WLC_SET_PM, 0))
        return false;
    if (!setVar("roam_off", on, sizeof(on)))
        IOLog("SwitchWLAN: roam_off refused: %s\n", why_);
    multicastApply();
    if (!setVar("join_pref", joinPref, sizeof(joinPref)))
        IOLog("SwitchWLAN: join_pref refused: %s\n", why_);
    if (!cmdInt(WLC_UP, 1))
        return false;
    coexBringUp();
    coexConfigure();
    uint8_t country[12] = {};
    size_t n = 0;
    char abbrev[5] = "?";
    char code[5] = "?";
    if (getVar("country", country, sizeof(country), &n) && n >= 12) {
        memcpy(abbrev, country, 4);
        abbrev[4] = 0;
        memcpy(code, country + 8, 4);
        code[4] = 0;
    }
    IOLog("SwitchWLAN: radio up, MAC %02x:%02x:%02x:%02x:%02x:%02x, country %s/%d (%s)\n", mac_[0], mac_[1], mac_[2],
          mac_[3], mac_[4], mac_[5], abbrev, static_cast<int>(swlan::le32(country + 4)), code);
    if (capAwdl_)
        awdlProbe();  // once a boot, while the radio is up and before the station uses it
#if DEVELOPMENT || DEBUG
    monitorProbe();
#endif
    locked([&] {
        status_.state = powerOn_ ? SWLAN_STA_IDLE : SWLAN_STA_OFF;
        status_.power = powerOn_;
    });
    if (!powerOn_)
        cmdInt(WLC_DOWN, 1);
    return true;
}

// ---- bring-up, watchdog, recovery --------------------------------------------------------------------------------

// Chip identity, firmware download and start, rings, MSI: shared by the first bring-up and every recovery.
bool SwitchWLAN::bootChip(const char **stage)
{
    *stage = "chip";
    if (!chip_.attach(&pcie_, ep_.bar0, ep_.bar2, why_, sizeof(why_)))
        return false;
    static uint8_t nvram[4096];
    const size_t nvramLength =
        swlan::nvramConvert(switch_wlan_fw_nvram, switch_wlan_fw_nvram_size, nvram, sizeof(nvram));
    *stage = "firmware";
    if (!nvramLength) {
        snprintf(why_, sizeof(why_), "NVRAM (%llu bytes of text) does not convert", switch_wlan_fw_nvram_size);
        return false;
    }
    if (!chip_.boot(switch_wlan_fw_image, switch_wlan_fw_image_size, nvram, nvramLength, why_, sizeof(why_)))
        return false;
    *stage = "rings";
    if (!mb_.attach(&chip_, &SwitchWLAN::onEvent, &SwitchWLAN::onFrame, this, why_, sizeof(why_)))
        return false;
    char layout[200];
    mb_.describe(layout, sizeof(layout));
    IOLog("SwitchWLAN: msgbuf: %s\n", layout);
    *stage = "MSI";
    if (!pcie_.msiEnable(mb_.msiPA(), why_, sizeof(why_)))
        return false;
    attachInterrupt();
    locked([&] {
        gone_ = halted_ = false;
        up_ = true;
        mb_.enableInterrupts();
    });
    *stage = "ioctl ver";
    uint8_t ver[256] = {};
    size_t n = 0;
    if (!getVar("ver", ver, sizeof(ver) - 1, &n))
        return false;
    for (size_t i = 0; i < n; ++i) {
        if (!ver[i] || ver[i] == '\n') {
            ver[i] = 0;
            break;
        }
        if (ver[i] < 0x20 || ver[i] >= 0x7F)
            ver[i] = '?';
    }
    IOLog("SwitchWLAN: firmware: %s (%llu MSIs so far)\n", reinterpret_cast<const char *>(ver),
          static_cast<unsigned long long>(interrupts_));
    // The chanspec layout (brcmf_cfg80211_attach's BRCMF_C_GET_VERSION): d11ac unless the firmware says d11n.
    uint8_t io[4] = {};
    size_t ioLength = 0;
    if (ioctl(WLC_GET_VERSION, nullptr, 0, sizeof(io), io, &ioLength) && ioLength >= 4)
        iotype_ = swlan::le32(io) == SWLAN_IOTYPE_D11N ? SWLAN_IOTYPE_D11N : SWLAN_IOTYPE_D11AC;
    // What this firmware build was compiled with, as brcmfmac reads it (brcmf_feat_firmware_capabilities): "cap"
    // names p2p, mchan, monitor, rtap, awdl and nan when present. AirDrop needs AWDL, or failing that monitor mode with
    // injection; the image's own strings cannot tell (most of the firmware is in the chip's ROM). Read once; a refusal
    // is logged and changes nothing.
    uint8_t cap[768] = {};
    size_t capLength = 0;
    if (getVar("cap", cap, sizeof(cap) - 1, &capLength)) {
        for (size_t i = 0; i < capLength; ++i) {
            if (!cap[i])
                break;
            if (cap[i] < 0x20 || cap[i] >= 0x7F)
                cap[i] = ' ';
        }
        IOLog("SwitchWLAN: firmware capabilities: %s\n", reinterpret_cast<const char *>(cap));
        // the firmware's own AWDL (r7p8: present on 7.35.252.108); its iovars are probed after the bring-up
        capAwdl_ = false;
        for (size_t i = 0; i + 4 <= capLength && cap[i]; ++i) {
            if (!memcmp(cap + i, "awdl", 4) && (i == 0 || cap[i - 1] == ' ') && (cap[i + 4] == ' ' || !cap[i + 4]))
                capAwdl_ = true;
        }
        __atomic_store_n(&capRead_, true, __ATOMIC_RELEASE);
    } else {
        IOLog("SwitchWLAN: firmware capabilities not read: %s\n", why_);
    }
    *stage = "radio setup";
    return radioInit();
}

bool SwitchWLAN::bringUp()
{
    IOLog("SwitchWLAN: starting, bringing up PCIe port 1\n");
    if (!pcie_.bringUp(ep_, why_, sizeof(why_))) {
        stop("PCIe");
        return false;
    }
    IOLog("SwitchWLAN: PCIe link up on port 1 (Gen%u x%u): %04x:%04x, BAR0 %#x BAR2 %#x\n", ep_.linkSpeed,
          ep_.linkWidth, ep_.vendor, ep_.device, ep_.bar0, ep_.bar2);
    if (ep_.vendor != 0x14E4 || ep_.device != 0x43EC) {
        snprintf(why_, sizeof(why_), "endpoint %04x:%04x is not the BCM4356 (14e4:43ec)", ep_.vendor, ep_.device);
        stop("PCIe");
        return false;
    }
    const char *stage = "chip";
    if (!bootChip(&stage)) {
        stop(stage);
        return false;
    }
    IOLog("SwitchWLAN: BCM%x rev %u, %u cores, RAM %u KiB at %#x\n", chip_.id(), chip_.rev(), chip_.coreCount(),
          chip_.ramSize() / 1024, chip_.ramBase());
    return true;
}

// Once a second while the chip runs: what no interrupt tells (nxk `check`).
void SwitchWLAN::watchdog()
{
    char why[400] = {};
    locked([&] {
        if (!up_)
            return;
        if (gone_) {
            snprintf(why, sizeof(why), "chip reads all ones (interrupt)");
            return;
        }
        const uint32_t flags = chip_.liveFlags();
        if (flags == 0xFFFFFFFF) {
            snprintf(why, sizeof(why), "chip reads all ones");
        } else if (halted_ || (flags & swlan::Chip::kSharedDied)) {
            char crash[200];
            char console[160];
            chip_.crash(crash, sizeof(crash));
            chip_.consoleTail(console, sizeof(console));
            snprintf(why, sizeof(why), "firmware died (%s); console: %s", crash, console);
        } else if (!pcie_.endpointDecodes()) {
            snprintf(why, sizeof(why), "chip reset itself (memory decoding off)");
        }
    });
    if (why[0])
        lost(why);
}

// The chip stopped: reported, the rings abandoned (nothing is sent to it until it is back), the link down.
void SwitchWLAN::lost(const char *why)
{
    bool wasUp = false;
    locked([&] {
        up_ = false;
        wasUp = status_.state == SWLAN_STA_UP;
        status_.state = SWLAN_STA_OFF;
        ++losses_;
    });
    __atomic_store_n(&gInfraFreq, 0u, __ATOMIC_RELEASE);
    char state[200];
    pcie_.describe(state, sizeof(state));
    IOLog("SwitchWLAN: chip lost (%u so far): %s [%s]\n", losses_, why, state);
    secured_ = false;
    sup_.wipe();
    if (wasUp && events_ && events_->link_lost)
        events_->link_lost(-1);
    bool awdl = false;
    locked([&] {
        awdl = awdlOpen_;
        awdlIfidx_ = awdlBssidx_ = -1;  // the firmware's AWDL interface went with it; rings rebuilt at the next attach
    });
    if (awdl && awdlLossFn_) {
        const swlan_awdl_event e = {SWLAN_AWDL_CHIP_LOST, 0, 0, 0};
        awdlLossFn_(awdlLossCtx_, &e);
    }
}

// nxk `recover`: module reset under PERST#, link, endpoint (the same BARs), firmware, rings, radio; then the last
// network again with the key wifid gave for it.
bool SwitchWLAN::recover(bool afterSleep)
{
    swlan::TegraPcie::Endpoint ep = {};
    if (!(afterSleep ? pcie_.bringUp(ep, why_, sizeof(why_)) : pcie_.restart(ep, why_, sizeof(why_)))) {
        sleptRecover_ = afterSleep;  // the next try needs the whole bring-up too
        return false;
    }
    if (ep.bar0 != ep_.bar0 || ep.bar2 != ep_.bar2) {
        snprintf(why_, sizeof(why_), "BARs moved to %#x and %#x", ep.bar0, ep.bar2);
        return false;
    }
    const char *stage = "chip";
    if (!bootChip(&stage))
        return false;
    ++recoveries_;
    IOLog("SwitchWLAN: chip back after loss %u\n", losses_);
    locked([&] {
        if (haveLast_ && powerOn_ && !joinRequested_) {
            join_ = last_;
            joinRequested_ = true;
        }
    });
    return true;
}

// ---- system sleep -------------------------------------------------------------------------------------------------
//
// SC7 powers the PCIe controller, its PHY and PLLE off; the chip's firmware state does not survive the bring-up after
// it (moduleReset power-cycles it, as switchroot's brcm,reset_on_wake does), so the chip is let go before sleep and
// brought up whole after it, back on the last network with the key wifid gave (recover). The worker parks for the
// sleep: nothing touches the controller from the sleep action on.
void SwitchWLAN::parkForSleep()
{
    bool wasUp = false;
    locked([&] {
        wasUp = up_;
    });
    if (wasUp)
        lost("system sleep");
    if (msi_)
        msi_->disable();
    locked([&] {
        sleep_ = kParked;
        gate_->commandWakeup(&sleep_);
        while (sleep_ == kParked)
            gate_->commandSleep(&sleep_, THREAD_UNINT);
        sleep_ = kAwake;
    });
    if (msi_)
        msi_->enable();
}

void SwitchWLAN::systemSleep()
{
    bool parked = false;
    locked([&] {
        if (!workerRunning_)
            return;
        sleep_ = kSleepAsked;
        kick();
        const uint64_t deadline = deadlineIn(3000);
        while (sleep_ == kSleepAsked && gate_->commandSleep(&sleep_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
        }
        parked = sleep_ == kParked;
    });
    const char *outcome = workerRunning_ ? "the worker did not park in 3 s" : "not running";
    if (parked)
        outcome = "chip let go, worker parked";
    switch_power_journal("SwitchWLAN: sleep: %s", outcome);
}

void SwitchWLAN::systemWake()
{
    locked([&] {
        if (sleep_ == kParked) {
            sleep_ = kWakeAsked;
            gate_->commandWakeup(&sleep_);
        } else if (sleep_ == kSleepAsked) {
            sleep_ = kAwake;
        }
    });
}

// ---- scans -------------------------------------------------------------------------------------------------------

// nxk wl.rs `scan`: an escan of every channel (or of one network), active, 3 probes, 80 ms active / 200 ms passive
// per channel (with the firmware's defaults nxk never heard a 5 GHz network). Results arrive in the work loop.
bool SwitchWLAN::startScan(const uint8_t *ssid, size_t ssidLength)
{
    uint8_t request[72] = {};
    swlan::put32(request + 0, 1);  // BRCMF_ESCAN_REQ_VERSION
    swlan::put16(request + 4, 1);  // WL_ESCAN_ACTION_START
    if (ssid && ssidLength && ssidLength <= 32) {
        swlan::put32(request + 8, static_cast<uint32_t>(ssidLength));
        memcpy(request + 12, ssid, ssidLength);
    }
    memset(request + 44, 0xFF, 6);           // any BSSID
    request[50] = 2;                         // DOT11_BSSTYPE_ANY
    request[51] = 0;                         // active
    swlan::put32(request + 52, 3);           // nprobes
    swlan::put32(request + 56, 80);          // active time, ms
    swlan::put32(request + 60, 200);         // passive time, ms
    swlan::put32(request + 64, 0xFFFFFFFF);  // home time: firmware default
    swlan::put32(request + 68, 0);           // every channel
    locked([&] {
        bssCount_ = 0;
        scanDone_ = false;
        scanning_ = true;
        if (status_.state == SWLAN_STA_IDLE)
            status_.state = SWLAN_STA_SCANNING;
        swlan::put16(request + 6, ++scanSync_);
    });
    scanEndNs_ = swlan::nowNs() + kScanTimeoutMs * 1000000ULL;
    if (setVar("escan", request, sizeof(request)))
        return true;
    IOLog("SwitchWLAN: scan refused: %s\n", why_);
    locked([&] {
        scanning_ = false;
        scanDone_ = true;
        if (status_.state == SWLAN_STA_SCANNING)
            status_.state = SWLAN_STA_IDLE;
    });
    return false;
}

// An escan whose last result (status other than partial) has not come in the time a full scan takes: the firmware's
// scan engine is stopped (brcmf_notify_escan_complete with fw_abort: WLC_SCAN on channel -1) and the scan ends as
// aborted with what came, so the next request can start. Without this one lost event would wedge scanning for good.
void SwitchWLAN::abortScan()
{
    IOLog("SwitchWLAN: scan: no final escan result in %u ms, aborting it\n", kScanTimeoutMs);
    uint8_t params[68] = {};       // brcmf_scan_params_le
    memset(params + 36, 0xFF, 6);  // any BSSID
    params[42] = 2;                // DOT11_BSSTYPE_ANY
    swlan::put32(params + 44, 1);  // nprobes
    swlan::put32(params + 48, 0xFFFFFFFF);
    swlan::put32(params + 52, 0xFFFFFFFF);
    swlan::put32(params + 56, 0xFFFFFFFF);
    swlan::put32(params + 60, 1);       // one channel ...
    swlan::put16(params + 64, 0xFFFF);  // ... -1: abort
    if (!ioctl(WLC_SCAN, params, sizeof(params), 0, nullptr, nullptr))
        IOLog("SwitchWLAN: scan abort: %s\n", why_);
    locked([&] {
        scanStatus_ = 4;  // WLC_E_STATUS_ABORT
        scanDone_ = true;
        abortedSync_ = scanSync_;
    });
}

void SwitchWLAN::finishScan()
{
    unsigned count = 0;
    uint32_t status = 0;
    locked([&] {
        scanning_ = false;
        count = bssCount_;
        status = scanStatus_;
        if (status_.state == SWLAN_STA_SCANNING)
            status_.state = SWLAN_STA_IDLE;
    });
    static unsigned logged;
    if (logged++ < 4 || status)
        IOLog("SwitchWLAN: scan: %u networks%s\n", count, status ? " (scan aborted)" : "");
    if (events_ && events_->scan_done)
        events_->scan_done();
}

bool SwitchWLAN::scanAndWait(const uint8_t *ssid, size_t ssidLength)
{
    if (!startScan(ssid, ssidLength))
        return false;
    bool done = false;
    while (!done && swlan::nowNs() < scanEndNs_) {
        waitWork(200);
        locked([&] {
            done = scanDone_;
        });
    }
    if (!done)
        abortScan();
    finishScan();
    return done;
}

// ---- joins -------------------------------------------------------------------------------------------------------

// The access point the firmware associated with (brcmf_update_bss_info: WLC_GET_BSS_INFO answers a length word, then
// one brcmf_bss_info_le). Without a BSSID from wifid the firmware's own join scan picks the access point, which
// need not be the one in our table: r3v joined a -42 dBm access point on channel 1 while the table held only the
// same network's -71 dBm one on channel 6. Also kept in the scan table, so wifid's next cached scan has it.
bool SwitchWLAN::readJoinedBss(swlan_bss &out)
{
    constexpr size_t kRoom = 1500;  // msgbuf ioctls carry at most 1518 bytes
    uint8_t *info = static_cast<uint8_t *>(IOMallocZeroData(kRoom));
    if (!info)
        return false;
    swlan::put32(info, kRoom);
    size_t n = 0;
    const bool ok =
        ioctl(WLC_GET_BSS_INFO, info, 4, kRoom, info, &n) && n > 4 && parseBss(info + 4, n - 4, out, iotype_);
    // The access point's HT/VHT/HE from every element it sent (the table keeps only the first 384 bytes of them).
    peer_ = {};
    if (ok) {
        const uint32_t ieOffset = swlan::le16(info + 4 + 116);
        const uint32_t ieLength = swlan::le32(info + 4 + 120);
        if (4 + ieOffset + ieLength <= n)
            swlan_peer_parse(info + 4 + ieOffset, ieLength, &peer_);
    }
    IOFreeData(info, kRoom);
    if (ok) {
        locked([&] {
            keepBss(out, swlan::nowNs());
        });
    }
    return ok;
}

bool SwitchWLAN::popEvent(FwEvent &event)
{
    bool got = false;
    locked([&] {
        if (!fwCount_)
            return;
        event = fwEvents_[fwHead_];
        fwHead_ = (fwHead_ + 1) % 16;
        --fwCount_;
        got = true;
    });
    return got;
}

bool SwitchWLAN::popEapol(EapolFrame &frame)
{
    bool got = false;
    locked([&] {
        if (!eapolCount_)
            return;
        frame = eapol_[eapolHead_];
        eapolHead_ = (eapolHead_ + 1) % kEapolMax;
        --eapolCount_;
        got = true;
    });
    return got;
}

// The station's TX flow ring, created on first need and kept for the firmware's run (nxk `open_flow`).
bool SwitchWLAN::ensureFlow(const uint8_t destination[6])
{
    swlan::Msgbuf::Flow flow = swlan::Msgbuf::FLOW_CLOSED;
    locked([&] {
        if (mb_.flow() == swlan::Msgbuf::FLOW_CLOSED || mb_.flow() == swlan::Msgbuf::FLOW_REFUSED)
            mb_.flowCreateSubmit(destination, mac_);
        const uint64_t deadline = deadlineIn(kFlowWaitMs);
        while (mb_.flow() == swlan::Msgbuf::FLOW_PENDING &&
               gate_->commandSleep(&mb_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
        }
        flow = mb_.flow();
    });
    if (flow != swlan::Msgbuf::FLOW_OPEN) {
        IOLog("SwitchWLAN: TX flow ring %s (status %u)\n",
              flow == swlan::Msgbuf::FLOW_REFUSED ? "refused" : "not created in time", mb_.flowStatus());
    }
    return flow == swlan::Msgbuf::FLOW_OPEN;
}

uint32_t SwitchWLAN::sendFrame(const uint8_t *frame, size_t length)
{
    if (!ensureFlow(frame))
        return 0;
    uint32_t id = 0;
    locked([&] {
        id = up_ ? mb_.send(frame, length) : 0;
    });
    return id;
}

// A handshake reply on the air before its keys are installed (brcmf_netdev_wait_pend8021x): message 4 leaves in the
// clear.
bool SwitchWLAN::waitSent(uint32_t id)
{
    bool done = false;
    uint16_t status = 0;
    locked([&] {
        const uint64_t deadline = deadlineIn(kTxWaitMs);
        while (!(done = mb_.sent(id, &status)) &&
               gate_->commandSleep(&mb_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
        }
    });
    if (done && status)
        IOLog("SwitchWLAN: handshake frame sent with status %u\n", status);
    return done;
}

// A CCMP key to the firmware (brcmf_cfg80211_add_key, nxk `add_key`): pairwise for the access point, or a group key
// by index with its receive counter.
bool SwitchWLAN::addKey(uint32_t index, const uint8_t key[16], const uint8_t *peer, const uint8_t *rsc)
{
    uint8_t k[kKeySize] = {};
    swlan::put32(k + 0, index);
    swlan::put32(k + 4, 16);
    memcpy(k + 8, key, 16);
    swlan::put32(k + kKeyAlgo, kAlgoAesCcm);
    if (peer) {
        memcpy(k + kKeyEa, peer, 6);
    } else {
        swlan::put32(k + kKeyFlags, kPrimaryKey);
    }
    if (rsc) {
        // Receive IV: bits 16-47 of the packet number, then bits 0-15.
        memcpy(k + kKeyRxIv, rsc + 2, 4);
        memcpy(k + kKeyRxIv + 4, rsc, 2);
        k[kKeyIvInit] = 1;
    }
    const bool ok = setVar("wsec_key", k, sizeof(k));
    memset(k, 0, sizeof(k));
    if (!ok)
        IOLog("SwitchWLAN: %s key not installed: %s\n", peer ? "pairwise" : "group", why_);
    return ok;
}

// One EAPOL frame through the supplicant (nxk `apply`). True once pairwise and group keys are installed.
bool SwitchWLAN::handleEapol(const uint8_t *frame, size_t length)
{
    if (!secured_)
        return false;
    static swlan::wpa::Step step;  // worker only; 450 bytes off the stack
    if (const char *why = sup_.receive(frame, length, step)) {
        static unsigned said;
        if (said++ < 8)
            IOLog("SwitchWLAN: EAPOL-Key frame dropped: %s\n", why);
        return false;
    }
    if (step.replyLength) {
        const uint32_t id = sendFrame(step.reply, step.replyLength);
        if (!id) {
            IOLog("SwitchWLAN: handshake reply not sent\n");
        } else if (step.hasPtk || step.hasGtk) {
            waitSent(id);
        }
    }
    if (step.hasPtk)
        addKey(0, step.tk, sup_.aa(), nullptr);
    if (step.hasGtk)
        addKey(step.gtk.index, step.gtk.key, nullptr, step.gtk.rsc);
    memset(&step, 0, sizeof(step));
    return sup_.keyed();
}

// Leave the network (brcmf_link_down: WLC_DISASSOC with reason 3, "leaving").
void SwitchWLAN::leave()
{
    uint8_t scb[12] = {};
    swlan::put32(scb, kReasonLeaving);
    ioctl(WLC_DISASSOC, scb, sizeof(scb), 0, nullptr, nullptr);
    secured_ = false;
    sup_.wipe();
    locked([&] {
        if (status_.state != SWLAN_STA_OFF)
            status_.state = SWLAN_STA_IDLE;
    });
}

// The firmware event that ended the link, named: a deauthentication or disassociation the access point sent (its
// 802.11 reason code), one the firmware sent itself, or a link-down (brcmf WLC_E_LINK reasons: 1 beacons lost,
// 2 disassociated, 6 reassociation for a roam failed).
void SwitchWLAN::linkDown(const FwEvent &e)
{
    const char *what = linkDownCause(e.type);
    char counts[200];
    counters(counts, sizeof(counts));
    IOLog("SwitchWLAN: link lost: %s, reason %u (status %u, flags %#x); %s\n", what, e.reason, e.status, e.flags,
          counts);
    secured_ = false;
    sup_.wipe();
    setState(SWLAN_STA_IDLE);
    __atomic_store_n(&gInfraFreq, 0u, __ATOMIC_RELEASE);
    if (events_ && events_->link_lost)
        events_->link_lost(static_cast<int>(e.reason));
}

// nxk wl.rs `join`, the security chosen from what the network announced: open, or WPA2-Personal (CCMP, PSK) with
// the PMK wifid derived or one from the passphrase. Returns SWLAN_RES_*.
int SwitchWLAN::joinOnce(const JoinParams &p, char *ssidText)
{
    // The network in the last scan, or a directed scan for it (a hidden network answers only by name).
    swlan_bss target = {};
    bool found = false;
    const auto lookup = [&] {
        locked([&] {
            int best = -1;
            for (unsigned i = 0; i < bssCount_; ++i) {
                const swlan_bss &b = bss_[i];
                if (b.ssid_len != p.ssidLength || memcmp(b.ssid, p.ssid, p.ssidLength))
                    continue;
                if (p.hasBssid && memcmp(b.bssid, p.bssid, 6))
                    continue;
                if (best < 0 || b.signal_dbm > bss_[best].signal_dbm)
                    best = static_cast<int>(i);
            }
            if (best >= 0) {
                target = bss_[best];
                found = true;
            }
        });
    };
    lookup();
    if (!found) {
        scanAndWait(p.ssid, p.ssidLength);
        lookup();
    }
    if (!found) {
        IOLog("SwitchWLAN: join '%s': not in range\n", ssidText);
        return SWLAN_RES_NOT_FOUND;
    }
    size_t rsnLength = 0;
    const uint8_t *rsn = swlan::wpa::element(target.ie, target.ie_len, 48, &rsnLength);
    bool wpa1 = false;
    for (size_t at = 0; at + 2 <= target.ie_len; at += 2 + target.ie[at + 1]) {
        if (target.ie[at] == 221 && target.ie[at + 1] >= 4 && at + 6 <= target.ie_len && target.ie[at + 2] == 0x00 &&
            target.ie[at + 3] == 0x50 && target.ie[at + 4] == 0xF2 && target.ie[at + 5] == 0x01)
            wpa1 = true;
    }
    const bool privacy = (target.caps & (1u << 4)) != 0u;
    const bool secured = rsn != nullptr;
    if (!secured && (wpa1 || privacy)) {
        IOLog("SwitchWLAN: join '%s': %s networks are not supported\n", ssidText, wpa1 ? "WPA (TKIP)" : "WEP");
        return SWLAN_RES_UNSUPPORTED;
    }
    uint8_t pmk[32] = {};
    if (secured) {
        if (const char *why = swlan::wpa::checkRsn(rsn, rsnLength)) {
            IOLog("SwitchWLAN: join '%s': %s\n", ssidText, why);
            return SWLAN_RES_UNSUPPORTED;
        }
        if (!p.hasKey)
            return SWLAN_RES_BAD_KEY;
        if (p.keyIsPmk && p.keyLength == 32) {
            memcpy(pmk, p.key, 32);
        } else if (!swlan::crypto::pmkFromHex(p.key, p.keyLength, pmk)) {
            if (p.keyLength < 8 || p.keyLength > 63) {
                IOLog("SwitchWLAN: join '%s': passphrase of %zu characters, 8 to 63 needed\n", ssidText, p.keyLength);
                return SWLAN_RES_BAD_KEY;
            }
            swlan::crypto::passphrasePmk(p.key, p.keyLength, p.ssid, p.ssidLength, pmk);
        }
        sup_.init(pmk, mac_, randomBytes);
        memset(pmk, 0, sizeof(pmk));
    }
    secured_ = secured;

    // Stale events and EAPOL frames (an earlier scan or join) must not decide this one.
    locked([&] {
        fwCount_ = eapolCount_ = 0;
        status_.state = SWLAN_STA_AUTH;
    });
    const uint32_t wpaAuth = secured ? kWpa2AuthPsk : 0;
    const uint32_t wsec = secured ? kAesEnabled : 0;
    const uint32_t zero = 0;
    uint8_t le[4];
    bool ok = setVar("wpaie", secured ? swlan::wpa::kRsnIe : nullptr, secured ? sizeof(swlan::wpa::kRsnIe) : 0);
    if (!ok)
        IOLog("SwitchWLAN: wpaie: %s\n", why_);  // the firmware may build its own element
    swlan::put32(le, wpaAuth);
    ok = setVar("wpa_auth", le, 4);
    swlan::put32(le, zero);
    ok = ok && setVar("auth", le, 4);
    swlan::put32(le, wsec);
    ok = ok && setVar("wsec", le, 4);
    // brcmf_ext_join_params_le: SSID, join scan with firmware defaults, the BSSID (or any), no channel list.
    uint8_t joinParams[68] = {};
    swlan::put32(joinParams, static_cast<uint32_t>(p.ssidLength));
    memcpy(joinParams + 4, p.ssid, p.ssidLength);
    joinParams[36] = 0xFF;  // scan type: default
    for (int i = 0; i < 4; ++i)
        swlan::put32(joinParams + 40 + static_cast<ptrdiff_t>(4) * i, 0xFFFFFFFF);
    if (p.hasBssid) {
        memcpy(joinParams + 56, p.bssid, 6);
    } else {
        memset(joinParams + 56, 0xFF, 6);
    }
    ok = ok && setVar("join", joinParams, sizeof(joinParams));
    if (!ok) {
        IOLog("SwitchWLAN: join '%s' not started: %s\n", ssidText, why_);
        return SWLAN_RES_FAILED;
    }

    bool associated = false;
    bool link = false;
    bool keyed = !secured;
    bool ieRead = false;
    bool announced = false;
    uint8_t bssid[6] = {};
    const uint64_t end = swlan::nowNs() + kJoinTimeoutMs * 1000000ULL;
    while (swlan::nowNs() < end) {
        FwEvent e;
        while (popEvent(e)) {
            const bool hasAddr =
                (static_cast<unsigned>(e.addr[0]) | e.addr[1] | e.addr[2] | e.addr[3] | e.addr[4] | e.addr[5]) != 0;
            switch (e.type) {
            case E_SET_SSID:
                if (e.status) {
                    static const char *const failures[] = {"", "failed", "timed out", "no such network", "aborted"};
                    IOLog("SwitchWLAN: join '%s' %s (status %u)\n", ssidText,
                          e.status < 5 ? failures[e.status] : "failed", e.status);
                    if (e.status == 3)
                        return SWLAN_RES_NOT_FOUND;
                    return e.status == 2 ? SWLAN_RES_TIMEOUT : SWLAN_RES_FAILED;
                }
                associated = true;
                if (hasAddr)
                    memcpy(bssid, e.addr, 6);
                break;
            case E_ASSOC:
                if (!e.status && hasAddr)
                    memcpy(bssid, e.addr, 6);
                break;
            case E_LINK:
                link = ((e.flags & kLinkUp) != 0u);
                if (link && hasAddr)
                    memcpy(bssid, e.addr, 6);
                break;
            case E_DEAUTH:
            case E_DEAUTH_IND:
            case E_DISASSOC:
            case E_DISASSOC_IND:
                if (secured && sup_.unconfirmed()) {
                    IOLog("SwitchWLAN: join '%s': key handshake refused, wrong password? (reason %u)\n", ssidText,
                          e.reason);
                    return SWLAN_RES_BAD_KEY;
                }
                IOLog("SwitchWLAN: join '%s': sent away by the access point (reason %u)\n", ssidText, e.reason);
                return SWLAN_RES_REJECTED;
            default:
                break;
            }
        }
        if (associated && link && !announced) {
            // Associated: the access point as the firmware has it, then the network side (wifid takes the link up now and
            // holds it for its clients until the join completes, connect_done, after the key handshake).
            announced = true;
            swlan_bss joined = target;
            if (!readJoinedBss(joined)) {
                IOLog("SwitchWLAN: join '%s': the joined access point's record is unreadable: %s\n", ssidText, why_);
                if (static_cast<unsigned>(bssid[0]) | bssid[1] | bssid[2] | bssid[3] | bssid[4] | bssid[5])
                    memcpy(joined.bssid, bssid, 6);
            }
            locked([&] {
                *current_ = joined;
                memcpy(status_.bssid, joined.bssid, 6);
                memcpy(status_.ssid, p.ssid, p.ssidLength);
                status_.ssid_len = static_cast<uint8_t>(p.ssidLength);
                status_.freq_mhz = joined.freq_mhz;
                status_.signal_dbm = joined.signal_dbm;
                status_.bw_mhz = joined.bw_mhz;
                status_.center = joined.center;
                status_.tx_rate_kbps = status_.rx_rate_kbps = 0;
                status_.state = secured && !keyed ? SWLAN_STA_KEYS : SWLAN_STA_ASSOC;
            });
            if (events_ && events_->associated)
                events_->associated();
        }
        EapolFrame f;
        while (secured && popEapol(f)) {
            // Message 2 repeats the RSN element the association request carried: the firmware's, if it built one
            // (brcmf_get_assoc_ies: assoc_info gives the request's IE length, assoc_req_ies the IEs).
            if (!ieRead) {
                ieRead = true;
                uint8_t info[64] = {};
                uint8_t ies[512] = {};
                size_t infoLength = 0;
                size_t iesLength = 0;
                if (getVar("assoc_info", info, sizeof(info), &infoLength) && infoLength >= 4 &&
                    getVar("assoc_req_ies", ies, sizeof(ies), &iesLength)) {
                    size_t used = swlan::le32(info);
                    if (used > iesLength)
                        used = iesLength;
                    size_t n = 0;
                    if (const uint8_t *ours = swlan::wpa::element(ies, used, 48, &n))
                        sup_.setIe(ours, n);
                }
            }
            keyed |= handleEapol(f.frame, f.length);
        }
        bool leaving = false;
        locked([&] {
            leaving = leaveRequested_;
        });
        if (leaving)
            return SWLAN_RES_FAILED;
        if (associated && link && keyed) {
            // The firmware may turn power saving on when it associates; PM 0 until the join is done, then coexJoined
            // sets the mode the station runs (coexApply: HOS's PM 2 with its 2-5 s sleep return while Bluetooth is on).
            cmdInt(WLC_SET_PM, 0);
            ensureFlow(bssid);
            uint32_t mhz = 0;
            int32_t rssi = 0;
            uint8_t answer[4] = {};
            size_t n = 0;
            uint8_t scb[12] = {};
            const bool fresh = ioctl(WLC_GET_RSSI, scb, sizeof(scb), sizeof(answer), answer, &n) && n >= 4;
            locked([&] {
                if (fresh) {
                    const int32_t r = static_cast<int32_t>(swlan::le32(answer));
                    status_.signal_dbm = current_->signal_dbm = clampDbm(r);
                }
                mhz = status_.freq_mhz;
                rssi = status_.signal_dbm;
                status_.state = SWLAN_STA_UP;
                last_ = p;
                haveLast_ = true;
            });
            __atomic_store_n(&gInfraFreq, mhz, __ATOMIC_RELEASE);
            IOLog("SwitchWLAN: joined '%s' (%s) at %u MHz, %d dBm\n", ssidText, secured ? "WPA2" : "open", mhz, rssi);
            coexJoined();
            logLink();
            // en0's frames can go now: its start routine runs (it found no room while the station was not up).
            locked([&] {
                if (events_ && events_->tx_ready)
                    events_->tx_ready();
            });
            nextRssiNs_ = swlan::nowNs() + kRssiRefreshMs * 1000000ULL;
            nextStatsNs_ = swlan::nowNs() + kStatsMs * 1000000ULL;
            snapshot(lastCounts_);
            statsSinceNs_ = swlan::nowNs();
            return SWLAN_RES_OK;
        }
        waitWork(100);
    }
    if (secured && sup_.unconfirmed()) {
        IOLog("SwitchWLAN: join '%s': no message 3, wrong password?\n", ssidText);
        return SWLAN_RES_BAD_KEY;
    }
    const char *handshake = "none";
    if (secured)
        handshake = sup_.hasAa() ? "started" : "not started";
    IOLog("SwitchWLAN: join '%s' timed out (associated %d, link %d, keyed %d, handshake %s)\n", ssidText,
          static_cast<int>(associated), static_cast<int>(link), static_cast<int>(keyed), handshake);
    return SWLAN_RES_TIMEOUT;
}

void SwitchWLAN::join(const JoinParams &p)
{
    char ssidText[33];
    printable(p.ssid, p.ssidLength, ssidText);
    bool wasScanning = false;
    locked([&] {
        wasScanning = scanning_;
    });
    if (wasScanning) {
        // A scan in progress finishes first (the firmware refuses a join meanwhile).
        bool done = false;
        while (!done && swlan::nowNs() < scanEndNs_) {
            waitWork(200);
            locked([&] {
                done = scanDone_;
            });
        }
        if (!done)
            abortScan();
        finishScan();
    }
    const char *with = "";
    if (p.hasKey)
        with = p.keyIsPmk ? " with the PMK wifid gave" : " with a passphrase";
    IOLog("SwitchWLAN: joining '%s'%s\n", ssidText, with);
    const int result = joinOnce(p, ssidText);
    if (result != SWLAN_RES_OK) {
        uint8_t scb[12] = {};
        swlan::put32(scb, kReasonLeaving);
        ioctl(WLC_DISASSOC, scb, sizeof(scb), 0, nullptr, nullptr);
        secured_ = false;
        sup_.wipe();
    }
    locked([&] {
        status_.result = result;
        if (result != SWLAN_RES_OK && status_.state != SWLAN_STA_OFF)
            status_.state = SWLAN_STA_IDLE;
    });
    if (events_ && events_->connect_done)
        events_->connect_done(result);
}

// ---- the worker ---------------------------------------------------------------------------------------------------

void SwitchWLAN::run()
{
    if (!bringUp())
        return;
    locked([&] {
        workerRunning_ = true;
    });
    switch_wlan_net_start(this, loop_, mac_);
    netStarted_ = true;
    switch_wlan_awdl_capable();  // a radio without AWDL gets its down awdl0 before userspace asks for it
    IOLog("SwitchWLAN: en0 ready, %u MSIs so far\n", static_cast<unsigned>(interrupts_));

    uint64_t nextWatchdog = 0;
    uint64_t retryAt = 0;
    uint32_t backoff = kRetryFirstMs;
    uint32_t tickMs = kWatchdogMs;
    for (;;) {
        waitWork(tickMs);
        uint8_t sleeping = kAwake;
        locked([&] {
            sleeping = sleep_;
        });
        if (sleeping == kSleepAsked) {
            parkForSleep();
            retryAt = 0;  // SC7 powered the controller off: the whole bring-up, at once
            backoff = kRetryFirstMs;
            sleptRecover_ = true;
            continue;
        }
        const uint64_t now = swlan::nowNs();
        bool chipUp = false;
        bool scanning = false;
        locked([&] {
            chipUp = up_;
            scanning = scanning_ || scanRequested_;
        });
        tickMs = chipUp && !scanning ? kIdleTickMs : kWatchdogMs;
        if (chipUp && now >= nextWatchdog) {
            nextWatchdog = now + tickMs * 1000000ULL;
            watchdog();
            locked([&] {
                chipUp = up_;
            });
            if (!chipUp) {
                retryAt = now + kRetryFirstMs * 1000000ULL;
                backoff = kRetryFirstMs;
            }
        }
        if (!chipUp) {
            if (now < retryAt)
                continue;
            const bool afterSleep = sleptRecover_;
            sleptRecover_ = false;
            if (recover(afterSleep)) {
                backoff = kRetryFirstMs;
            } else {
                IOLog("SwitchWLAN: recovery failed: %s; next try in %u s\n", why_, backoff / 1000);
                retryAt = swlan::nowNs() + backoff * 1000000ULL;
                backoff = backoff * 2 > kRetryMaxMs ? kRetryMaxMs : backoff * 2;
                continue;
            }
        }

        // Requests from the network side.
        bool scan = false;
        bool doJoin = false;
        bool doLeave = false;
        bool doPower = false;
        bool powerOn = true;
        bool scanFinished = false;
        bool scanOverdue = false;
        uint8_t scanSsid[32];
        size_t scanSsidLength = 0;
        JoinParams params = {};
        locked([&] {
            scan = scanRequested_ && !scanning_;
            if (scan) {
                scanRequested_ = false;
                memcpy(scanSsid, scanSsid_, sizeof(scanSsid));
                scanSsidLength = scanSsidLength_;
            }
            doJoin = joinRequested_;
            if (doJoin) {
                params = join_;
                memset(join_.key, 0, sizeof(join_.key));
                joinRequested_ = false;
            }
            doLeave = leaveRequested_;
            leaveRequested_ = false;
            doPower = powerRequested_;
            powerRequested_ = false;
            powerOn = powerOn_;
            scanFinished = scanning_ && scanDone_;
            scanOverdue = scanning_ && !scanDone_ && now >= scanEndNs_;
        });
        if (doPower) {
            if (!powerOn) {
                leave();
                cmdInt(WLC_DOWN, 1);
                setState(SWLAN_STA_OFF);
            } else {
                cmdInt(WLC_UP, 1);
                setState(SWLAN_STA_IDLE);
            }
            locked([&] {
                status_.power = powerOn;
            });
        }
        if (doLeave)
            leave();
        if (scanOverdue)
            abortScan();
        if (scanFinished || scanOverdue)
            finishScan();
        if (!powerOn)
            continue;
        if (doJoin) {
            join(params);
            memset(&params, 0, sizeof(params));
            continue;
        }
        if (scan)
            startScan(scanSsidLength ? scanSsid : nullptr, scanSsidLength);

        // While joined: link loss and group rekeys; what wifid said Bluetooth now does (coexApply).
        uint32_t state = SWLAN_STA_OFF;
        bool btChanged = false;
        locked([&] {
            state = status_.state;
            btChanged = btChanged_;
        });
        if (btChanged) {
            coexMode();
            if (state == SWLAN_STA_UP)
                coexApply();
        }
        bool mcastChanged = false;
        locked([&] {
            mcastChanged = mcastChanged_;
        });
        if (mcastChanged)
            multicastApply();
        FwEvent e;
        while (popEvent(e)) {
            if (state != SWLAN_STA_UP)
                continue;
            const bool down = (e.type == E_LINK && ((e.flags & kLinkUp) == 0u)) || e.type == E_DEAUTH ||
                              e.type == E_DEAUTH_IND || e.type == E_DISASSOC || e.type == E_DISASSOC_IND;
            if (down) {
                linkDown(e);
                state = SWLAN_STA_IDLE;
            }
        }
        EapolFrame f;
        while (popEapol(f)) {
            if (state == SWLAN_STA_UP)
                handleEapol(f.frame, f.length);
        }
        if (state == SWLAN_STA_UP && swlan::nowNs() >= nextStatsNs_) {
            nextStatsNs_ = swlan::nowNs() + kStatsMs * 1000000ULL;
#if DEVELOPMENT || DEBUG
            logStats();
#else
            refreshRates();
#endif
        }
        if (state == SWLAN_STA_UP && swlan::nowNs() >= nextRssiNs_) {
            nextRssiNs_ = swlan::nowNs() + kRssiRefreshMs * 1000000ULL;
            uint8_t scb[12] = {};
            uint8_t answer[4] = {};
            size_t n = 0;
            if (ioctl(WLC_GET_RSSI, scb, sizeof(scb), sizeof(answer), answer, &n) && n >= 4) {
                const int32_t rssi = static_cast<int32_t>(swlan::le32(answer));
                locked([&] {
                    status_.signal_dbm = current_->signal_dbm = clampDbm(rssi);
                });
            }
        }
    }
}

// While joined, every 5 s that saw traffic: what the data path moved and lost in that window. Received: frames from
// the firmware, those passed to en0, dropped for want of an mbuf, refused by the stack's input. Sent: frames posted,
// their completions, posts refused (flow ring closed, all 32 buffers in flight, ring full), if_output drops (too
// large, or refused here). Plus the MSIs taken and the signal.
void SwitchWLAN::snapshot(Counts &c)
{
    locked([&] {
        c.rx = mb_.framesReceived();
        c.tx = mb_.framesSent();
        c.done = mb_.sendsCompleted();
        c.refused = mb_.sendsRefused();
        c.failed = mb_.sendsFailed();
        c.rxGroup = rxGroup_;
        c.rxUnicast = rxUnicast_;
        c.rxBytes = rxBytes_;
        c.txBytes = txBytes_;
        c.msis = interrupts_;
        c.data = mb_.takeCounters();
        switch_wlan_net_trace(&c.trace);
    });
    switch_wlan_net_counters(&c.net);
}

#if DEVELOPMENT || DEBUG
void SwitchWLAN::logStats()
{
    Counts now;
    snapshot(now);
    const Counts &was = lastCounts_;
    const uint64_t nowNs = swlan::nowNs();
    if (!statsSinceNs_)
        statsSinceNs_ = nowNs;
    if (now.rx == was.rx && now.tx == was.tx && now.refused == was.refused &&
        now.net.tx_oversize == was.net.tx_oversize) {
        lastCounts_ = now;
        statsSinceNs_ = nowNs;
        return;
    }
    // r16-idle: an idle trickle (fewer than kStatsTrickleFrames frames since the last line, none dropped, refused or
    // failed: beacons' broadcasts, mDNS, a keepalive) is carried over to one line a minute, with the firmware queries
    // below; real traffic or any trouble is logged every kStatsMs as before.
    const bool trouble = now.refused != was.refused || now.failed != was.failed ||
                         now.net.tx_oversize != was.net.tx_oversize || now.net.rx_no_mbuf != was.net.rx_no_mbuf ||
                         now.net.rx_input_errors != was.net.rx_input_errors;
    // A Bluetooth link up is coexistence's case (Wi-Fi quiet while the shared antenna serves Bluetooth): never carried
    // over. SwitchBluetooth's snapshot reads "N ACL[, M LE][, active ...]", the rest only with links: so a link is a
    // count that is not 0 or anything after it.
    switch_coex_bt btState;
    char btLinks[96];
    bool btLink = false;
    if (switch_bluetooth_coex(&btState, btLinks, sizeof(btLinks))) {
        btLink = btLinks[0] != '0';
        for (const char *c = btLinks; *c && !btLink; ++c)
            btLink = *c == ',';
    }
    const bool trickle = (now.rx - was.rx) + (now.tx - was.tx) < kStatsTrickleFrames && !btLink;
    if (!trouble && trickle && nowNs - statsSinceNs_ < kStatsQuietMs * 1000000ULL)
        return;  // keep counting
    const uint32_t windowMs = static_cast<uint32_t>((nowNs - statsSinceNs_) / 1000000ULL);
    statsSinceNs_ = nowNs;
    int signal = 0;
    uint32_t btFlags = 0;
    bool btPower = true;
    locked([&] {
        signal = status_.signal_dbm;
        btFlags = btFlags_;
        btPower = btPower_;
    });
    char out[1024];  // periodic: to the log only (os_log), never the boot console
    const int n = snprintf(
        out, sizeof(out),
        "SwitchWLAN: stats %u ms: rx %u (en0 %u: unicast %u group %u, %llu KB, no mbuf %u, input refused %u), "
        "tx %u (%llu KB) done %u failed %u refused %u, oversize %u, %llu MSIs, %d dBm",
        windowMs, now.rx - was.rx, now.net.rx_input - was.net.rx_input, now.rxUnicast - was.rxUnicast,
        now.rxGroup - was.rxGroup, static_cast<unsigned long long>((now.rxBytes - was.rxBytes) / 1024),
        now.net.rx_no_mbuf - was.net.rx_no_mbuf, now.net.rx_input_errors - was.net.rx_input_errors, now.tx - was.tx,
        static_cast<unsigned long long>((now.txBytes - was.txBytes) / 1024), now.done - was.done,
        now.failed - was.failed, now.refused - was.refused, now.net.tx_oversize - was.net.tx_oversize,
        static_cast<unsigned long long>(now.msis - was.msis), signal);
    size_t at = n > 0 ? static_cast<size_t>(n) : 0;
    const auto advance = [&](int k) {
        at += k > 0 ? static_cast<size_t>(k) : 0;
    };
    if (at < sizeof(out) && now.failed != was.failed)
        advance(snprintf(out + at, sizeof(out) - at, " (status %u)", mb_.lastFailStatus()));
    // r17-wlanperf, its own line after this one (the kernel's os_log carries about 1 KiB a line): what the data path
    // moved and what that cost. Mb/s each way (the radio's own byte counts), frames per MSI, doorbells and chip-RAM
    // index reads per frame (the PCIe round trips), the receive buffers the chip holds and those given back, the
    // transmit buffers in flight (peak of the window, times en0 found none free), en0's start-routine runs and
    // batches and its input batches, and the firmware's MPDUs per A-MPDU each way.
    char data[400];
    {
        const swlan::DataCounters &d = now.data;
        const swlan::DataCounters &e = was.data;
        const uint64_t rxB = d.rxBytes - e.rxBytes;
        const uint64_t txB = d.txBytes - e.txBytes;
        const uint64_t frames = (d.rxFrames - e.rxFrames) + (d.txDone - e.txDone);
        const uint64_t ms = windowMs ? windowMs : 1;
        char rxMbps[16];
        char txMbps[16];
        char perMsi[16];
        char bells[16];
        char reads[16];
        char ampdu[64];
        swlan_tenths(rxB * 8, ms * 1000, rxMbps, sizeof(rxMbps));
        swlan_tenths(txB * 8, ms * 1000, txMbps, sizeof(txMbps));
        swlan_tenths(frames, now.msis - was.msis, perMsi, sizeof(perMsi));
        swlan_tenths((d.doorbells - e.doorbells) * 100, frames, bells, sizeof(bells));
        swlan_tenths((d.indexReads - e.indexReads) * 100, frames, reads, sizeof(reads));
        ampduWindow(ampdu, sizeof(ampdu));
        snprintf(data, sizeof(data),
                 "SwitchWLAN: data %u ms: rx %s tx %s Mb/s, %s frames/MSI, per 100 frames %s doorbells %s index reads, "
                 "RX %u/%u posted (%llu reposted), TX %u in flight (peak %u of %u, ran out %llu), en0 "
                 "%u starts %u batches, %u input batches, %s",
                 windowMs, rxMbps, txMbps, perMsi, bells, reads, d.rxPosted, mb_.rxBufferCount(),
                 static_cast<unsigned long long>(d.rxReposted - e.rxReposted), d.txInFlight, d.txPeak,
                 mb_.txBufferCount(), static_cast<unsigned long long>(d.txFull - e.txFull),
                 now.net.tx_starts - was.net.tx_starts, now.net.tx_batches - was.net.tx_batches,
                 now.net.rx_batches - was.net.rx_batches, ampdu);
    }
    // The access point's view of the same window (sta_info): what it acknowledged and what it sent us.
    swlan_sta sta;
    if (readSta(sta)) {
        char text[200];
        swlan_sta_describe(&staLast_, &sta, text, sizeof(text));
        if (text[0] && at < sizeof(out))
            advance(snprintf(out + at, sizeof(out) - at, "; %s", text));
        staLast_ = sta;
        // The link's rates for wifid (APPLE80211_IOC_RATE) and en0's baud rate.
        locked([&] {
            status_.tx_rate_kbps = sta.tx_rate_kbps;
            status_.rx_rate_kbps = sta.rx_rate_kbps;
        });
        switch_wlan_net_rate(static_cast<uint64_t>(sta.tx_rate_kbps) * 1000);
    }
    // The firmware's time in power save: with PM 0 set by us, only coexistence's own protection puts it there.
    if (!pmDurRefused_) {
        uint8_t v[4] = {};
        size_t got = 0;
        if (getVar("pm_dur", v, sizeof(v), &got) && got >= 4) {
            const uint32_t pm = swlan::le32(v);
            if (pmDurHave_ && at < sizeof(out)) {
                advance(snprintf(out + at, sizeof(out) - at, "; PS pm_dur %u (+%d ms)", pm,
                                 static_cast<int>(pm - pmDurLast_)));
            }
            pmDurLast_ = pm;
            pmDurHave_ = true;
        } else {
            pmDurRefused_ = true;
            os_log(OS_LOG_DEFAULT, "SwitchWLAN: coex: pm_dur not available: %s", why_);
        }
    }
    // The firmware's dynamic coexistence as it stands: the mode it runs and the desense it applies (btc_dynctl_status).
    if (!btcStatusRefused_) {
        uint8_t v[16] = {};
        size_t got = 0;
        swlan_btc_status btc;
        if (getVar("btc_dynctl_status", v, sizeof(v), &got) && swlan_btc_status_parse(v, got, &btc)) {
            char text[96];
            swlan_btc_status_describe(&btc, text, sizeof(text));
            if (at < sizeof(out))
                advance(snprintf(out + at, sizeof(out) - at, "; %s", text));
        } else {
            btcStatusRefused_ = true;
            os_log(OS_LOG_DEFAULT, "SwitchWLAN: coex: btc_dynctl_status not available (%zu bytes): %s", got, why_);
        }
    }
    // Bluetooth: what wifid says and what the controller was asked to do (SwitchBluetooth).
    char flags[48];
    char bt[200];
    char links[96];
    swlan_bt_flags_describe(btFlags, flags, sizeof(flags));
    switch_coex_bt state;
    if (switch_bluetooth_coex(&state, links, sizeof(links))) {
        switch_coex_describe(&state, bt, sizeof(bt));
        if (at < sizeof(out))
            advance(snprintf(out + at, sizeof(out) - at, "; BT %s, %s; wifid %s", bt, links, flags));
    } else if (at < sizeof(out)) {
        advance(snprintf(out + at, sizeof(out) - at, "; BT %s; wifid %s", btPower ? "no client" : "off", flags));
    }
    if (at < sizeof(out))
        switch_wlan_awdl_stat(out + at, sizeof(out) - at);
    os_log(OS_LOG_DEFAULT, "%s", out);
    os_log(OS_LOG_DEFAULT, "%s", data);
    logProbe(now, was, windowMs);
    lastCounts_ = now;
}

// Where an unanswered query was lost: "probe" has what crossed en0 for DNS and ARP, whether the transmit buffers held
// in memory what the CPU wrote, and the firmware's power-save state; "fw counters" has the firmware's receive drops
// that sta_info does not count (bad address, replay, undecryptable, duplicate) and its null/PS-Poll frames, raw by slot.
void SwitchWLAN::logProbe(const Counts &now, const Counts &was, uint32_t windowMs)
{
    char out[1024];
    const int n = snprintf(out, sizeof(out), "SwitchWLAN: probe %u ms: ", windowMs);
    size_t at = n > 0 ? static_cast<size_t>(n) : 0;
    const auto advance = [&](int k) {
        at += k > 0 ? static_cast<size_t>(k) : 0;
    };
    if (at < sizeof(out))
        advance(swlan_trace_describe(&now.trace, &was.trace, out + at, sizeof(out) - at));
    const swlan::DataCounters &d = now.data;
    const swlan::DataCounters &e = was.data;
    if (at < sizeof(out)) {
        advance(snprintf(out + at, sizeof(out) - at, "; TX buffers read back %llu, %llu differ",
                         static_cast<unsigned long long>(d.txReread - e.txReread),
                         static_cast<unsigned long long>(d.txRereadDiffer - e.txRereadDiffer)));
    }
    if (at < sizeof(out) && d.txRereadDiffer != e.txRereadDiffer)
        advance(snprintf(out + at, sizeof(out) - at, " (first at byte %u)", d.txRereadAt));
    uint8_t v[24] = {};
    size_t got = 0;
    int pm = -1;
    if (!pmReadRefused_) {
        if (ioctl(WLC_GET_PM, nullptr, 0, 4, v, &got) && got >= 4) {
            pm = static_cast<int>(swlan::le32(v));
        } else {
            pmReadRefused_ = true;
        }
    }
    int low = -1;
    int high = -1;
    int logic = -1;
    if (!sleepRetReadRefused_) {
        got = 0;
        if (getVar("pm2_sleep_ret_ext", v, sizeof(v), &got) && got >= 6) {
            logic = v[0];
            low = v[2] | (static_cast<unsigned>(v[3]) << 8);
            high = v[4] | (static_cast<unsigned>(v[5]) << 8);
        } else {
            sleepRetReadRefused_ = true;
        }
    }
    int btcdyn = -1;
    got = 0;
    uint8_t profile[64] = {};
    if (getVar("btc_dynctl", profile, sizeof(profile), &got) && got >= 2 && profile[0] == 1)
        btcdyn = profile[1];
    if (at < sizeof(out)) {
        advance(snprintf(out + at, sizeof(out) - at,
                         "; firmware PM %d, sleep return logic %d %d/%d ms, btcdyn flags %d", pm, logic, low, high,
                         btcdyn));
    }
    os_log(OS_LOG_DEFAULT, "%s", out);

    if (fwCountersRefused_)
        return;
    uint8_t *block = fwCountersNow_;
    got = 0;
    if (!getVar("counters", block, 1518, &got) || got < 4) {
        fwCountersRefused_ = true;
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: fw counters not available (%zu bytes): %s", got, why_);
        return;
    }
    char text[900];
    swlan_fw_counters_delta(block, got, fwCountersLength_ ? fwCounters_ : nullptr, fwCountersLength_, text,
                            sizeof(text));
    os_log(OS_LOG_DEFAULT, "SwitchWLAN: fw counters %u ms: %s", windowMs, text);
    memcpy(fwCounters_, block, got);
    fwCountersLength_ = got;
}

#else
void SwitchWLAN::refreshRates()
{
    const uint32_t frames = mb_.framesReceived() + mb_.framesSent();
    if (frames == ratesFrames_)
        return;
    ratesFrames_ = frames;
    swlan_sta sta;
    if (!readSta(sta))
        return;
    locked([&] {
        status_.tx_rate_kbps = sta.tx_rate_kbps;
        status_.rx_rate_kbps = sta.rx_rate_kbps;
    });
    switch_wlan_net_rate(static_cast<uint64_t>(sta.tx_rate_kbps) * 1000);
}
#endif

// The access point's counters (sta_info by its address). A firmware that refuses or answers in a layout not known
// here (SwitchWLANCoex.h) gets one line and is not asked again this boot.
bool SwitchWLAN::readSta(swlan_sta &out)
{
    if (staRefused_)
        return false;
    uint8_t bssid[6] = {};
    locked([&] {
        memcpy(bssid, status_.bssid, 6);
    });
    uint8_t info[256] = {};
    size_t n = 0;
    if (!getVarWith("sta_info", bssid, sizeof(bssid), info, sizeof(info), &n)) {
        staRefused_ = true;
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: coex: sta_info not available: %s", why_);
        return false;
    }
    if (!swlan_sta_parse(info, n, &out) || !(out.have & SWLAN_STA_V3)) {
        staRefused_ = true;
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: coex: sta_info version %u length %u (%zu bytes) not understood", out.ver,
               out.len, n);
        return false;
    }
    return true;
}

bool SwitchWLAN::varInt(const char *name, uint32_t &value)
{
    uint8_t v[4] = {};
    size_t n = 0;
    if (!getVar(name, v, sizeof(v), &n) || n < 4)
        return false;
    value = swlan::le32(v);
    return true;
}

bool SwitchWLAN::varIntWith(const char *name, uint32_t param, uint32_t &value)
{
    uint8_t p[4];
    uint8_t v[4] = {};
    swlan::put32(p, param);
    size_t n = 0;
    if (!getVarWith(name, p, sizeof(p), v, sizeof(v), &n) || n < 4)
        return false;
    value = swlan::le32(v);
    return true;
}

// Once per join (os_log): what the link runs at and what it could. The firmware's chanspec and transmit rate, the
// access point's HT/VHT/HE from its beacon, our side (nmode, vhtmode, bw_cap per band, the chains, A-MPDU and
// A-MSDU), and the best PHY rate both ends allow at the width joined. "-" for whatever the firmware does not answer.
void SwitchWLAN::logLink()
{
    char text[700];
    size_t at = 0;
    auto add = [&](int k) {
        at += k > 0 ? static_cast<size_t>(k) : 0;
    };
    auto room = [&] {
        return at < sizeof(text) ? sizeof(text) - at : 0;
    };
    const auto num = [&](const char *label, bool ok, uint32_t v, bool hex) {
        if (!room())
            return;
        if (!ok) {
            add(snprintf(text + at, room(), "%s -", label));
        } else {
            add(snprintf(text + at, room(), hex ? "%s %#x" : "%s %u", label, v));
        }
    };
    uint32_t v = 0;
    swlan_chanspec cs = {};
    const bool haveCs = varInt("chanspec", v) && swlan_chanspec_decode(static_cast<uint16_t>(v), iotype_, &cs);
    uint8_t rate[4] = {};
    size_t n = 0;
    // 500 kb/s units; anything past 10 Gb/s (the firmware's -1 for "auto", say) is no rate
    const bool haveRate = ioctl(WLC_GET_RATE, nullptr, 0, sizeof(rate), rate, &n) && n >= 4 &&
                          (swlan::le32(rate) != 0u) && swlan::le32(rate) <= 20000;
    const uint32_t rateKbps = haveRate ? swlan::le32(rate) * 500 : 0;
    if (haveRate) {
        locked([&] {
            status_.tx_rate_kbps = rateKbps;
        });
    }
    if (haveRate)
        switch_wlan_net_rate(static_cast<uint64_t>(rateKbps) * 1000);
    const swlan_peer ap = peer_;
    char peer[120];
    swlan_peer_describe(&ap, peer, sizeof(peer));
    if (haveCs) {
        add(snprintf(text, sizeof(text), "SwitchWLAN: link: %s channel %u, %u MHz (centre %u), ",
                     cs.five_ghz ? "5 GHz" : "2.4 GHz", cs.control, cs.bw_mhz, cs.center));
    } else {
        add(snprintf(text, sizeof(text), "SwitchWLAN: link: chanspec %s, ", why_));
    }
    if (room()) {
        if (haveRate) {
            add(snprintf(text + at, room(), "rate %u.%u Mb/s", rateKbps / 1000, (rateKbps % 1000) / 100));
        } else {
            add(snprintf(text + at, room(), "rate -"));
        }
    }
    const uint32_t best = swlan_link_best_kbps(&ap, haveCs ? cs.bw_mhz : 20);
    if (room())
        add(snprintf(text + at, room(), " (best both ends allow here %u Mb/s); AP: %s; ours:", best / 1000, peer));
    uint32_t value = 0;
    bool ok = varInt("nmode", value);
    num(" nmode", ok, value, false);
    ok = varInt("vhtmode", value);
    num(" vhtmode", ok, value, false);
    ok = varIntWith("bw_cap", 2, value);  // WLC_BAND_2G
    num(" bw_cap 2.4 GHz", ok, value, true);
    ok = varIntWith("bw_cap", 1, value);  // WLC_BAND_5G
    num(" 5 GHz", ok, value, true);
    ok = varInt("txchain", value);
    num(", chains tx", ok, value, true);
    ok = varInt("rxchain", value);
    num(" rx", ok, value, true);
    ok = varInt("ampdu", value);
    num("; A-MPDU", ok, value, false);
    ok = varInt("ampdu_ba_wsize", value);
    num(" window", ok, value, false);
    ok = varInt("ampdu_mpdu", value);
    num(" mpdu", ok, value, false);
    ok = varInt("amsdu", value);
    num(", A-MSDU", ok, value, false);
    os_log(OS_LOG_DEFAULT, "%s", text);
}

#if DEVELOPMENT || DEBUG
// The firmware's MPDUs per A-MPDU since the last stats line, each way, from its "dump ampdu" text (wlc_ampdu_dump's
// txampdu/txmpdu/rxampdu/rxmpdu counters). A firmware without the dump, or without those counters in it, gets one
// line with what it answered and is not asked again.
void SwitchWLAN::ampduWindow(char *out, size_t size)
{
    snprintf(out, size, "A-MPDU -");
    if (ampduRefused_)
        return;
    constexpr size_t kRoom = 1500;  // msgbuf ioctls carry at most 1518 bytes; the counters lead the dump
    char *text = static_cast<char *>(IOMallocZeroData(kRoom + 1));
    if (!text)
        return;
    size_t n = 0;
    static const char kAmpdu[] = "ampdu";
    const bool ok = getVarWith("dump", kAmpdu, sizeof(kAmpdu), reinterpret_cast<uint8_t *>(text), kRoom, &n);
    uint32_t v[4] = {};
    static const char *const names[4] = {"txampdu", "txmpdu", "rxampdu", "rxmpdu"};
    bool found = ok;
    for (unsigned i = 0; i < 4 && found; ++i)
        found = swlan_dump_value(text, n, names[i], &v[i]);
    if (!found) {
        ampduRefused_ = true;
        for (size_t i = 0; i < n && i < 120; ++i) {
            if (text[i] && (text[i] < 0x20 || text[i] >= 0x7F))
                text[i] = ' ';
        }
        text[n < 120 ? n : 120] = 0;
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: dump ampdu %s: %s", ok ? "has no txampdu/txmpdu/rxampdu/rxmpdu" : "refused",
               ok ? text : why_);
    } else {
        if (ampduHave_) {
            char tx[16];
            char rx[16];
            swlan_tenths(v[1] - ampdu_[1], v[0] - ampdu_[0], tx, sizeof(tx));
            swlan_tenths(v[3] - ampdu_[3], v[2] - ampdu_[2], rx, sizeof(rx));
            snprintf(out, size, "A-MPDU tx %s rx %s MPDUs", tx, rx);
        }
        memcpy(ampdu_, v, sizeof(ampdu_));
        ampduHave_ = true;
    }
    IOFreeData(text, kRoom + 1);
}
#endif

// ---- Coexistence ----------------------------------------------------------------------------------------------
//
// The BCM4356's Wi-Fi and Bluetooth cores share one 2.4 GHz antenna (boardflags BFL_BTCOEX | BFL_FEM_BT) and arbitrate
// in hardware over the die's own GCI/SECI link, which the firmware sets up itself; the host's levers are the btc_*
// iovars and the power-save mode (SwitchWLANCoex.h). We run Nintendo's operating point for this firmware and NVRAM:
// the NVRAM's dynamic coexistence (btcdyn: hybrid mode 5, desense by Wi-Fi RSSI) unchanged, and PM 2 with HOS's
// sleep return while Bluetooth is on (coexApply). What the firmware actually runs is logged: its static settings once
// per bring-up (coexBringUp), PM and the policy once per join (coexJoined), the dynamic state, the access point's
// counters, the time in power save and Bluetooth's air use on every stats line (logStats).

// Bring-up (radioInit, after WLC_UP): the coexistence settings the firmware took from the NVRAM and its defaults, one
// line. Each value is "-" when the firmware does not answer it.
void SwitchWLAN::coexBringUp()
{
    char text[640];
    size_t at = 0;
    const auto add = [&](int k) {
        at += k > 0 ? static_cast<size_t>(k) : 0;
    };
    uint8_t v[64] = {};
    size_t n = 0;
    add(snprintf(text, sizeof(text), "SwitchWLAN: coex: firmware btc_mode "));
    if (getVar("btc_mode", v, sizeof(v), &n) && n >= 4) {
        add(snprintf(text + at, sizeof(text) - at, "%u", swlan::le32(v)));
    } else {
        add(snprintf(text + at, sizeof(text) - at, "-"));
    }
    // btc_params by index ("name\0" + le32 index -> le32): the NVRAM's two, SCO detection (27), the DHCP window's
    // (41, 66, 68) and eSCO's (50, 51) as brcmfmac and bcmdhd use them; btc_flags 0-8.
    static const uint32_t params[] = {27, 41, 50, 51, 66, 68, 84, 95};
    if (at < sizeof(text))
        add(snprintf(text + at, sizeof(text) - at, ", btc_params"));
    for (const uint32_t index : params) {
        uint8_t le[4];
        swlan::put32(le, index);
        if (at >= sizeof(text))
            break;
        if (getVarWith("btc_params", le, sizeof(le), v, sizeof(v), &n) && n >= 4) {
            add(snprintf(text + at, sizeof(text) - at, " %u=%#x", index, swlan::le32(v)));
        } else {
            add(snprintf(text + at, sizeof(text) - at, " %u=-", index));
        }
    }
    if (at < sizeof(text))
        add(snprintf(text + at, sizeof(text) - at, ", btc_flags 0-8"));
    for (uint32_t index = 0; index <= 8 && at < sizeof(text); ++index) {
        uint8_t le[4];
        swlan::put32(le, index);
        if (getVarWith("btc_flags", le, sizeof(le), v, sizeof(v), &n) && n >= 4) {
            add(snprintf(text + at, sizeof(text) - at, " %u", swlan::le32(v)));
        } else {
            add(snprintf(text + at, sizeof(text) - at, " -"));
        }
    }
    // The dynamic coexistence profile (btc_dynctl, 50 bytes): the NVRAM's btcdyn_* as the firmware holds them.
    char profile[200] = "";
    if (getVar("btc_dynctl", v, sizeof(v), &n))
        swlan_btc_profile_describe(v, n, profile, sizeof(profile));
    if (at < sizeof(text))
        add(snprintf(text + at, sizeof(text) - at, "; %s", profile[0] ? profile : "btcdyn -"));
    // The board's wiring as the NVRAM we loaded claims it (no SECI/GCI iovar exists: the link is on the die).
    const char *nvram = reinterpret_cast<const char *>(switch_wlan_fw_nvram);
    const size_t nvramLength = static_cast<size_t>(switch_wlan_fw_nvram_size);
    uint32_t flags = 0;
    uint32_t flags2 = 0;
    uint32_t mux = 0;
    const bool haveFlags = swlan_nvram_value(nvram, nvramLength, "boardflags", &flags);
    const bool haveFlags2 = swlan_nvram_value(nvram, nvramLength, "boardflags2", &flags2);
    const bool haveMux = swlan_nvram_value(nvram, nvramLength, "muxenab", &mux);
    if (at < sizeof(text)) {
        add(snprintf(text + at, sizeof(text) - at,
                     "; NVRAM boardflags %#x (BTCOEX %u, FEM_BT shared antenna %u) boardflags2 %#x (BT_SHARE_ANT0 %u, "
                     "BTCLEGACY %u: %s) muxenab %#x (SECI pins %u)",
                     haveFlags ? flags : 0, (flags & 0x1) ? 1u : 0u, (flags & 0x400000) ? 1u : 0u,
                     haveFlags2 ? flags2 : 0, (flags2 & 0x800000) ? 1u : 0u, (flags2 & 0x80) ? 1u : 0u,
                     (flags2 & 0x80) ? "legacy wires" : "GCI/SECI on the die", haveMux ? mux : 0,
                     (mux & 0x1000) ? 1u : 0u));
    }
    os_log(OS_LOG_DEFAULT, "%s", text);
}

// Any thread: what wifid says Bluetooth is doing (SwitchWLANApple80211.cpp, APPLE80211_IOC_BT_COEX_FLAGS / BT_POWER);
// one line per change, then the worker applies it (coexApply).
void SwitchWLAN::btState(uint32_t flags, bool power)
{
    bool changed = false;
    locked([&] {
        changed = flags != btFlags_ || power != btPower_;
        btFlags_ = flags;
        btPower_ = power;
        if (changed) {
            btChanged_ = true;
            kick();
        }
    });
    if (!changed)
        return;
    char text[48];
    swlan_bt_flags_describe(flags, text, sizeof(text));
    os_log(OS_LOG_DEFAULT, "SwitchWLAN: coex: wifid says Bluetooth %s, %s (flags %#x)", power ? "on" : "off", text,
           flags);
}

// Any thread (SwitchBluetooth's work loop): the controller's link count changed; the worker sets the mode (coexMode).
void SwitchWLAN::btLinks(unsigned links)
{
    locked([&] {
        if (links == btLinks_)
            return;
        btLinks_ = links;
        btChanged_ = true;
        kick();
    });
}

// Any thread (en0's SIOCADDMULTI, SIOCDELMULTI, SIOCSIFFLAGS): the stack's groups; the worker programs them.
void SwitchWLAN::multicast(const uint8_t (*groups)[6], unsigned count, bool all)
{
    if (count > SWLAN_MCAST_MAX) {
        count = 0;
        all = true;
    }
    locked([&] {
        if (count)
            memcpy(mcast_, groups, count * sizeof(mcast_[0]));
        mcastCount_ = count;
        mcastAll_ = all;
        mcastChanged_ = true;
        kick();
    });
}

// Worker and radioInit: the firmware passes up only the groups en0 joined (brcmf_set_multicast_list: "mcast_list"
// is a little-endian count and the addresses, then "allmulti"), so the CPU never wakes for another host's multicast.
// A list the firmware refuses passes every group instead, as brcmf does.
void SwitchWLAN::multicastApply()
{
    uint8_t list[4 + SWLAN_MCAST_MAX * 6];
    unsigned count = 0;
    bool all = false;
    locked([&] {
        count = mcastCount_;
        all = mcastAll_;
        memcpy(list + 4, mcast_, static_cast<size_t>(count) * 6);
        mcastChanged_ = false;
    });
    swlan::put32(list, count);
    if (!setVar("mcast_list", list, 4 + count * 6)) {
        IOLog("SwitchWLAN: mcast_list of %u groups refused (%s): every multicast frame passed up\n", count, why_);
        all = true;
    }
    uint8_t value[4];
    swlan::put32(value, all ? 1 : 0);
    if (!setVar("allmulti", value, sizeof(value)))
        IOLog("SwitchWLAN: allmulti refused: %s\n", why_);
}

// Bring-up (radioInit, after coexBringUp): the firmware's dynamic coexistence profile and mode kept as the idle state,
// then the state for the links there are (after a recovery Bluetooth may have some). One line.
void SwitchWLAN::coexConfigure()
{
    uint8_t v[64] = {};
    size_t n = 0;
    btcProfileHave_ = getVar("btc_dynctl", v, sizeof(v), &n) && n >= sizeof(btcProfile_) && v[0] == 1;
    if (btcProfileHave_)
        memcpy(btcProfile_, v, sizeof(btcProfile_));
    btcProfileFlags_ = btcProfileHave_ ? btcProfile_[1] : -1;
    btcProfileRefused_ = false;
    if (getVar("btc_mode", v, sizeof(v), &n) && n >= 4)
        btcIdleMode_ = swlan::le32(v);
    btcMode_ = btcIdleMode_;
    btcModeRefused_ = false;
    os_log(OS_LOG_DEFAULT,
           "SwitchWLAN: coex: dynamic coexistence (btcdyn flags %d) while Bluetooth has no link, off with one%s; "
           "idle mode %u",
           btcProfileFlags_, btcProfileHave_ ? "" : " (no version 1 profile: left as it is)", btcIdleMode_);
    coexMode();
}

// Worker, chip up: full TDM and the dynamic control off while Bluetooth has any link, the idle mode and the NVRAM's
// dynamic control without (swlan_coex_mode, swlan_btc_profile_for). The dynamic control goes off before full TDM is
// set and comes back only after the idle mode is, so its mode switching never overrides the mode we set. One line per
// change with the firmware's readback; a refusal is one line and that setting is left as it is for the boot.
void SwitchWLAN::coexMode()
{
    unsigned links = 0;
    locked([&] {
        links = btLinks_;
        btChanged_ = false;  // (coexApply, when joined, runs on the same change: the caller read the flag first)
    });
    if (links)
        coexProfile(links);
    const uint32_t want = swlan_coex_mode(links, btcIdleMode_);
    if (!btcModeRefused_ && want != btcMode_) {
        uint8_t le[4];
        swlan::put32(le, want);
        if (setVar("btc_mode", le, sizeof(le))) {
            btcMode_ = want;
            uint8_t v[4] = {};
            size_t n = 0;
            const int readBack = getVar("btc_mode", v, sizeof(v), &n) && n >= 4 ? static_cast<int>(swlan::le32(v)) : -1;
            os_log(OS_LOG_DEFAULT, "SwitchWLAN: coex: btc_mode %u (%s; firmware reads %d): %u Bluetooth link%s", want,
                   want == SWLAN_BTC_FULL_TDM ? "full TDM" : "the firmware's own", readBack, links,
                   links == 1 ? "" : "s");
        } else {
            btcModeRefused_ = true;
            os_log(OS_LOG_DEFAULT, "SwitchWLAN: coex: btc_mode %u refused (%s); left at %u", want, why_, btcMode_);
        }
    }
    if (!links)
        coexProfile(links);
}

void SwitchWLAN::coexProfile(unsigned links)
{
    uint8_t profile[sizeof(btcProfile_)];
    if (!btcProfileHave_ || btcProfileRefused_ ||
        !swlan_btc_profile_for(btcProfile_, sizeof(btcProfile_), links, profile) || profile[1] == btcProfileFlags_)
        return;
    if (!setVar("btc_dynctl", profile, sizeof(profile))) {
        btcProfileRefused_ = true;
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: coex: btc_dynctl flags %#x refused (%s); left at %d", profile[1], why_,
               btcProfileFlags_);
        return;
    }
    btcProfileFlags_ = profile[1];
    uint8_t back[64] = {};
    size_t n = 0;
    const int readBack = getVar("btc_dynctl", back, sizeof(back), &n) && n >= 2 && back[0] == 1 ? back[1] : -1;
    os_log(OS_LOG_DEFAULT,
           "SwitchWLAN: coex: dynamic coexistence %s (btcdyn flags %#x, firmware reads %d): %u Bluetooth "
           "link%s",
           profile[1] ? "on" : "off", profile[1], readBack, links, links == 1 ? "" : "s");
}

// Worker, once per join (joinOnce left the station at PM 0): the counters' baselines, then our power-save mode.
void SwitchWLAN::coexJoined()
{
    staLast_ = {};
    ampduHave_ = false;
    pmDurHave_ = false;
    pm_ = SWLAN_PM_OFF;
    sleepRetSet_ = false;
    coexApply();
}

// Worker, joined: the power-save mode for what Bluetooth does now (swlan_coex_pm): while it is on, HOS's
// pm2_sleep_ret_ext once per join and then PM 2; when it is off, PM 0. A firmware that refuses the sleep return stays
// at PM 0 (PM 2 with the firmware's default 200 ms sleep return would add latency), with one line. Every change is
// one line with the PM the firmware reads back.
void SwitchWLAN::coexApply()
{
    bool power = true;
    locked([&] {
        btChanged_ = false;
        power = btPower_;
    });
    if (power && !sleepRetSet_ && !sleepRetRefused_) {
        uint8_t ext[SWLAN_PM2_SLEEP_RET_EXT_SIZE];
        swlan_pm2_sleep_ret_ext(ext);
        if (setVar("pm2_sleep_ret_ext", ext, sizeof(ext))) {
            sleepRetSet_ = true;
        } else {
            sleepRetRefused_ = true;
            os_log(OS_LOG_DEFAULT, "SwitchWLAN: coex: pm2_sleep_ret_ext refused (%s); the station stays at PM 0", why_);
        }
    }
    const uint32_t want = swlan_coex_pm(power, sleepRetSet_);
    if (want == pm_)
        return;
    if (!cmdInt(WLC_SET_PM, want)) {
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: coex: PM %u refused: %s", want, why_);
        return;
    }
    pm_ = want;
    uint8_t v[4] = {};
    size_t n = 0;
    const int readBack =
        ioctl(WLC_GET_PM, nullptr, 0, sizeof(v), v, &n) && n >= 4 ? static_cast<int>(swlan::le32(v)) : -1;
    os_log(OS_LOG_DEFAULT, "SwitchWLAN: coex: PM %u (firmware reads %d)%s", want, readBack,
           want == SWLAN_PM_FAST ? ", sleep return 2000/5000 ms after 10 frames (HOS's): Bluetooth is on"
                                 : ": Bluetooth is off");
}

// ---- the station interface ---------------------------------------------------------------------------------------

void SwitchWLAN::setEvents(const swlan_events *events)
{
    locked([&] {
        events_ = events;
    });
}

int SwitchWLAN::requestScan(const uint8_t *ssid, size_t length)
{
    int result = 0;
    locked([&] {
        if (!up_ || !powerOn_) {
            result = -1;
            return;
        }
        scanRequested_ = true;
        scanSsidLength_ = ssid && length <= 32 ? length : 0;
        if (scanSsidLength_)
            memcpy(scanSsid_, ssid, scanSsidLength_);
        kick();
    });
    return result;
}

int SwitchWLAN::bssCount()
{
    int n = 0;
    locked([&] {
        n = static_cast<int>(bssCount_);
    });
    return n;
}

int SwitchWLAN::bssGet(int index, swlan_bss *out)
{
    int result = -1;
    locked([&] {
        if (index < 0 || static_cast<unsigned>(index) >= bssCount_)
            return;
        *out = bss_[index];
        out->age_ms = static_cast<uint32_t>((swlan::nowNs() - bssSeenNs_[index]) / 1000000);
        result = 0;
    });
    return result;
}

int SwitchWLAN::currentBss(swlan_bss *out)
{
    int result = -1;
    locked([&] {
        if (status_.state < SWLAN_STA_ASSOC)
            return;
        *out = *current_;
        out->age_ms = 0;
        result = 0;
    });
    return result;
}

int SwitchWLAN::requestJoin(const uint8_t *ssid, size_t ssidLength, const uint8_t *bssid, const uint8_t *key,
                            size_t keyLength, bool keyIsPmk)
{
    if (!ssid || !ssidLength || ssidLength > 32 || keyLength > 64)
        return -1;
    int result = 0;
    locked([&] {
        if (!up_ || !powerOn_) {
            result = -1;
            return;
        }
        JoinParams &p = join_;
        memset(&p, 0, sizeof(p));
        memcpy(p.ssid, ssid, ssidLength);
        p.ssidLength = ssidLength;
        p.hasBssid = bssid && (static_cast<unsigned>(bssid[0]) | bssid[1] | bssid[2] | bssid[3] | bssid[4] | bssid[5]);
        if (p.hasBssid)
            memcpy(p.bssid, bssid, 6);
        p.hasKey = key && keyLength;
        if (p.hasKey)
            memcpy(p.key, key, keyLength);
        p.keyLength = p.hasKey ? keyLength : 0;
        p.keyIsPmk = keyIsPmk;
        joinRequested_ = true;
        leaveRequested_ = false;
        status_.result = SWLAN_RES_FAILED;
        kick();
    });
    return result;
}

void SwitchWLAN::requestLeave()
{
    locked([&] {
        leaveRequested_ = true;
        haveLast_ = false;  // no rejoin after a recovery either
        memset(&last_, 0, sizeof(last_));
        kick();
    });
}

int SwitchWLAN::requestPower(bool on)
{
    locked([&] {
        powerOn_ = on;
        powerRequested_ = true;
        if (!on) {
            haveLast_ = false;
            memset(&last_, 0, sizeof(last_));
        }
        kick();
    });
    return 0;
}

// en0's output: straight onto the flow ring under the gate (if_output runs in thread context; the gate is the work
// loop's recursive lock, so a frame sent from the work loop's own thread is fine too).
int SwitchWLAN::transmit(const uint8_t *frame, size_t length)
{
    uint32_t id = 0;
    locked([&] {
        if (up_ && status_.state == SWLAN_STA_UP)
            id = mb_.send(frame, length);
        if (id)
            txBytes_ += length;
    });
    return id ? 0 : -1;
}

// en0's start routine (thread context): frames the station takes now; 0 while it is not up (the join's tx_ready
// starts en0 again) or every transmit buffer is in flight (Msgbuf::txResume, then tx_ready from the MSI path).
unsigned SwitchWLAN::txRoom()
{
    unsigned room = 0;
    locked([&] {
        if (up_ && status_.state == SWLAN_STA_UP)
            room = mb_.txRoom();
    });
    return room;
}

// en0's frames, under the gate: each written by `fill` straight into its transmit buffer, then one doorbell.
unsigned SwitchWLAN::transmitFrames(swlan_tx_fill_fn fill, void *context, unsigned max)
{
    unsigned taken = 0;
    locked([&] {
        if (!up_ || status_.state != SWLAN_STA_UP)
            return;
        const uint64_t now = swlan::nowNs();
        uint32_t readBackId = 0;
        size_t readBackLength = 0;
        while (taken < max && mb_.txRoom()) {
            uint8_t *buffer = mb_.txBuffer();
            const size_t length = buffer ? fill(context, buffer, swlan::kTxBufferSize) : 0;
            if (!length)
                break;
            const bool readBack = kTxReadBack && !readBackId && now >= nextReadBackNs_ &&
                                  length <= sizeof(txWritten_) * 8 &&
                                  swlan_frame_classify(buffer, length).kind == SWLAN_FRAME_DNS_QUERY;
            if (readBack) {
                readBackLength = length < sizeof(txWritten_) ? length : sizeof(txWritten_);
                memcpy(txWritten_, buffer, readBackLength);
            }
            const uint32_t id = mb_.txPost(length);
            if (!id)
                break;
            if (readBack)
                readBackId = id;
            txBytes_ += length;
            ++taken;
        }
        mb_.txFlush();
        if (readBackId) {
            mb_.txReadBack(readBackId, txWritten_, readBackLength);
            nextReadBackNs_ = now + 1000000000ULL;
        }
    });
    return taken;
}

void SwitchWLAN::status(swlan_status *out)
{
    locked([&] {
        *out = status_;
        memcpy(out->mac, mac_, 6);
    });
}

void SwitchWLAN::counters(char *text, size_t size)
{
    locked([&] {
        snprintf(text, size,
                 "rx %u (to en0 %u), tx %u done %u refused %u, eapol dropped %u, MSIs %llu, losses %u "
                 "recovered %u%s",
                 mb_.framesReceived(), rxToStack_, mb_.framesSent(), mb_.sendsCompleted(), mb_.sendsRefused(),
                 eapolDropped_, static_cast<unsigned long long>(interrupts_), losses_, recoveries_,
                 awdlOpen_ ? ", AWDL open" : "");
    });
}

// ---- the firmware's own AWDL (SwitchWLANAwdl.cpp) -----------------------------------------------------------------

// The events the firmware reports (brcmf_fweh "event_msgs": one bit per event), with the AWDL ones while AWDL is open.
bool SwitchWLAN::setEventMask(bool awdl)
{
    uint8_t mask[16] = {};
    for (const uint32_t e : kEventsWanted)
        mask[e / 8] |= static_cast<uint8_t>(1u << (e % 8));
    if (awdl) {
        for (unsigned i = 0; i < bcm_awdl_event_count; ++i)
            mask[bcm_awdl_events[i] / 8] |= static_cast<uint8_t>(1u << (bcm_awdl_events[i] % 8u));
    }
    return setVar("event_msgs", mask, sizeof(mask));
}

// Work loop, gate held: an AWDL event to the stack. WLC_E_IF names the AWDL interface's index (its frames and flow
// rings) or takes it away.
void SwitchWLAN::awdlEvent(const swlan::Event &event)
{
    ++awdlEventsIn_;
    if (event.type == E_IF && event.length >= 5) {
        const uint8_t ifidx = event.data[0];
        const uint8_t opcode = event.data[1];
        if (opcode == 1 && ifidx) {
            if (awdlIfidx_ >= 0 && awdlIfidx_ != ifidx)
                mb_.awdlClose();  // a stale interface's rings go first
            awdlIfidx_ = ifidx;
            awdlBssidx_ = event.data[3];
        } else if (opcode == 2 && static_cast<int>(ifidx) == awdlIfidx_) {
            awdlIfidx_ = awdlBssidx_ = -1;
            mb_.awdlClose();
        }
        const char *change = opcode == 2 ? "removed" : "changed";
        if (opcode == 1)
            change = "added";
        IOLog("SwitchWLAN: AWDL: firmware interface %u %s (bsscfg %u, role %u)\n", ifidx, change, event.data[3],
              event.data[4]);
    }
    if (switch_wlan_soft_event(event.type, event.status, event.addr, event.data, event.length))
        return;
    if (!awdlEventFn_)
        return;
    swlan_fw_event e = {event.type, event.status, event.reason, {}, event.ifidx, event.bsscfgidx};
    memcpy(e.addr, event.addr, 6);
    awdlEventFn_(awdlEventCtx_, &e, event.data, event.length);
}

// en0's capability answer: what awdlOpen would give, from what the bring-up already read (no firmware request).
int SwitchWLAN::awdlQuery(const swlan_awdl_caps **caps)
{
    static swlan_awdl_caps c;
    if (!__atomic_load_n(&capRead_, __ATOMIC_ACQUIRE))
        return -EAGAIN;  // the capability list is read at bring-up
    if (!capAwdl_)
        return -ENODEV;
    if (!__atomic_load_n(&gAwdlProbe.done, __ATOMIC_ACQUIRE))
        return -EAGAIN;  // the probe follows the first bring-up
    if (!mb_.awdlAvailable())
        return -ENOSPC;
    memset(&c, 0, sizeof(c));
    c.abi_version = SWLAN_AWDL_ABI_VERSION;
    c.has_native_awdl = 1;
    switch_wlan_soft_caps(&c);
    memcpy(c.self_mac, mac_, 6);
    c.probe = &gAwdlProbe;
    if (caps)
        *caps = &c;
    return 0;
}

int SwitchWLAN::awdlOpen(const swlan_awdl_caps **caps)
{
    if (loop_->onThread())
        return -EDEADLK;
    bool up = false;
    bool open = false;
    locked([&] {
        up = up_;
        open = awdlOpen_;
    });
    static swlan_awdl_caps c;
    if (!up) {
        IOLog("SwitchWLAN: AWDL: not opened: the radio is not up\n");
        return -ENXIO;
    }
    if (!capAwdl_) {
        IOLog("SwitchWLAN: AWDL: not opened: the firmware's capabilities do not list awdl\n");
        return -ENODEV;
    }
    if (!mb_.awdlAvailable()) {
        IOLog("SwitchWLAN: AWDL: not opened: no flow rings for an AWDL interface (DMA area or firmware limit)\n");
        return -ENOSPC;
    }
    if (!open) {
        locked([&] {
            awdlOpen_ = true;
        });
        if (!setEventMask(true)) {
            IOLog("SwitchWLAN: AWDL: not opened: the AWDL events were refused: %s\n", why_);
            locked([&] {
                awdlOpen_ = false;
            });
            return -EIO;
        }
        IOLog("SwitchWLAN: AWDL: firmware AWDL opened (AWDL events on, %u AWDL flow rings)\n", mb_.awdlRingCount());
    }
    switch_wlan_soft_init();
    memset(&c, 0, sizeof(c));
    c.abi_version = SWLAN_AWDL_ABI_VERSION;
    c.has_native_awdl = 1;
    switch_wlan_soft_caps(&c);
    memcpy(c.self_mac, mac_, 6);
    c.probe = gAwdlProbe.done ? &gAwdlProbe : nullptr;
    if (caps)
        *caps = &c;
    return 0;
}

#if DEVELOPMENT || DEBUG
// Once a boot (a probe the firmware does not survive is not retried by the recovery): every AWDL variable either
// firmware generation sets, read with GET on the station's interface. UNSUPPORTED means the name is not in the
// firmware's table; any other answer means its handler exists. The interface_create query is brcmf_feat_awdl_check's
// (version 3 = the series' AWDL interface). os_log only: status, bytes returned, the first 16 of them.
void SwitchWLAN::monitorProbe()
{
    static bool tried;
    if (tried)
        return;
    tried = true;
    for (unsigned i = 0; i < BCM_MONITOR_PROBE_COUNT; ++i) {
        uint8_t request[32];
        uint8_t reply[64] = {};
        size_t got = 0;
        int16_t fw = 0;
        const size_t n = bcm_monitor_probe_request(i, request, sizeof(request));
        const bool ok = i == BCM_MONITOR_PROBE_GET_MONITOR
                            ? ioctlOn(0, BCM_WLC_GET_MONITOR, nullptr, 0, 4, reply, &got, &fw)
                            : n && ioctlOn(0, WLC_GET_VAR, request, n, sizeof(reply), reply, &got, &fw);
        int status = fw ? fw : BCM_PROBE_NO_ANSWER;
        if (ok)
            status = 0;
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: monitor probe: %s: %d %s, %u bytes, first word %u",
               bcm_monitor_probe_names[i], status, bcme_name(status), static_cast<unsigned>(ok ? got : 0),
               static_cast<unsigned>(ok && got >= 4 ? swlan::le32(reply) : 0));
    }
}
#endif

void SwitchWLAN::awdlProbe()
{
    static bool tried;
    if (tried)
        return;
    tried = true;
#if DEVELOPMENT || DEBUG
    os_log(OS_LOG_DEFAULT, "SwitchWLAN: AWDL probe: %u variables, GET only", static_cast<unsigned>(BCM_PROBE_COUNT));
#endif
    for (unsigned i = 0; i < BCM_PROBE_COUNT; ++i) {
        uint8_t request[64];
        uint8_t reply[256] = {};
        const size_t n = bcm_probe_request(i, request, sizeof(request));
        size_t got = 0;
        int16_t fw = 0;
        const bool ok = (n != 0u) && ioctlOn(0, WLC_GET_VAR, request, n, sizeof(reply), reply, &got, &fw);
        int status = fw ? fw : BCM_PROBE_NO_ANSWER;
        if (ok)
            status = 0;
        bcm_probe_record(&gAwdlProbe, i, status, ok ? reply : nullptr, ok ? got : 0);
#if DEVELOPMENT || DEBUG
        char hex[3 * 16 + 1] = {};
        for (size_t k = 0; ok && k < got && k < 16; ++k)
            snprintf(hex + 3 * k, 4, " %02x", reply[k]);
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: AWDL probe: %s: %d %s, %u bytes%s", bcm_probe_names[i], status,
               bcme_name(status), static_cast<unsigned>(ok ? got : 0), hex);
#endif
    }
    __atomic_store_n(&gAwdlProbe.done, 1, __ATOMIC_RELEASE);  // awdlQuery reads it from another thread
    const enum bcm_awdl_api api = bcm_awdl_api_of(&gAwdlProbe);
    os_log(OS_LOG_DEFAULT,
           "SwitchWLAN: AWDL probe: API %s (interface_create version %u, awdl_if %s, slot_bss %s, awdl_chan_seq %s, "
           "mchan %s); %u of %u variables present",
           bcm_awdl_api_name(api), static_cast<unsigned>(gAwdlProbe.ifc_version),
           bcm_probe_has(&gAwdlProbe, BCM_PROBE_AWDL_IF) ? "yes" : "no",
           bcm_probe_has(&gAwdlProbe, BCM_PROBE_SLOT_BSS) ? "yes" : "no",
           bcm_probe_has(&gAwdlProbe, BCM_PROBE_AWDL_CHAN_SEQ) ? "yes" : "no",
           bcm_probe_has(&gAwdlProbe, BCM_PROBE_MCHAN) ? "yes" : "no",
           static_cast<unsigned>(BCM_PROBE_COUNT - __builtin_popcount(bcm_probe_absent(&gAwdlProbe))),
           static_cast<unsigned>(BCM_PROBE_COUNT));
}

void SwitchWLAN::awdlClose()
{
    bool was = false;
    locked([&] {
        was = awdlOpen_;
        awdlOpen_ = false;
        awdlIfidx_ = awdlBssidx_ = -1;
        if (up_)
            mb_.awdlClose();
    });
    if (was && !loop_->onThread())
        setEventMask(false);
}

// "<name>\0<value>" on firmware interface `ifidx` (brcmf_fil_iovar_data_set/get on that interface's ifp: the msgbuf
// ioctl's ifidx byte, no "bsscfg:" prefix, as the brcmfmac AWDL series sends every awdl_* variable). A firmware
// refusal comes back as SWLAN_FWERR(code) for the stack to report; only requests that never got an answer are
// logged here.
int SwitchWLAN::awdlVar(bool set, uint8_t ifidx, const char *name, const void *in, size_t inLength, void *out,
                        size_t outLength, size_t *returned)
{
    if (loop_->onThread())
        return -EDEADLK;
    uint8_t request[1024];
    const size_t nameLength = strlen(name) + 1;
    if (nameLength + inLength > sizeof(request))
        return -EMSGSIZE;
    memcpy(request, name, nameLength);
    if (inLength)
        memcpy(request + nameLength, in, inLength);
    const size_t n = nameLength + inLength;
    int16_t fw = 0;
    const bool ok =
        set ? ioctlOn(ifidx, WLC_SET_VAR, request, n, 0, nullptr, nullptr, &fw)
            : ioctlOn(ifidx, WLC_GET_VAR, request, n, outLength, static_cast<uint8_t *>(out), returned, &fw);
    if (ok)
        return 0;
    if (fw)
        return SWLAN_FWERR(fw);
    static unsigned said;
    if (said++ < 8)
        IOLog("SwitchWLAN: AWDL: %s %s (ifidx %u): %s\n", set ? "set" : "get", name, ifidx, why_);
    return -EIO;
}

int SwitchWLAN::awdlIoctlSet(uint32_t cmd, const void *buf, size_t length)
{
    if (loop_->onThread())
        return -EDEADLK;
    int16_t fw = 0;
    if (ioctlOn(0, cmd, static_cast<const uint8_t *>(buf), length, 0, nullptr, nullptr, &fw))
        return 0;
    return fw ? SWLAN_FWERR(fw) : -EIO;
}

int SwitchWLAN::awdlTransmit(const uint8_t *eth, size_t length)
{
    int r = -1;
    locked([&] {
        if (up_ && awdlOpen_ && awdlIfidx_ >= 0)
            r = mb_.awdlSend(static_cast<uint8_t>(awdlIfidx_), eth, length);
    });
    return r < 0 ? -1 : 0;
}

// Only stored (any thread, no gate): pointer-sized stores, the context before its function.
void SwitchWLAN::awdlCallbacks(swlan_fw_event_fn events, void *eventsContext, swlan_eth_rx_fn rx, void *rxContext,
                               swlan_evt_fn loss, void *lossContext)
{
    if (events) {
        __atomic_store_n(&awdlEventCtx_, eventsContext, __ATOMIC_RELEASE);
        __atomic_store_n(&awdlEventFn_, events, __ATOMIC_RELEASE);
    }
    if (rx) {
        __atomic_store_n(&awdlRxCtx_, rxContext, __ATOMIC_RELEASE);
        __atomic_store_n(&awdlRxFn_, rx, __ATOMIC_RELEASE);
    }
    if (loss) {
        __atomic_store_n(&awdlLossCtx_, lossContext, __ATOMIC_RELEASE);
        __atomic_store_n(&awdlLossFn_, loss, __ATOMIC_RELEASE);
    }
}

// ---- C entry points ------------------------------------------------------------------------------------------------

extern "C" uint16_t swlan_awdl_infra_freq(void)
{
    return static_cast<uint16_t>(__atomic_load_n(&gInfraFreq, __ATOMIC_ACQUIRE));
}

extern "C" void switch_wlan_set_events(const struct swlan_events *events)
{
    if (gDriver)
        gDriver->setEvents(events);
}

extern "C" int switch_wlan_scan(const uint8_t *ssid, size_t ssid_len)
{
    return gDriver ? gDriver->requestScan(ssid, ssid_len) : -1;
}

extern "C" int switch_wlan_bss_count(void)
{
    return gDriver ? gDriver->bssCount() : 0;
}

extern "C" int switch_wlan_bss_get(int index, struct swlan_bss *out)
{
    return gDriver ? gDriver->bssGet(index, out) : -1;
}

extern "C" int switch_wlan_current_bss(struct swlan_bss *out)
{
    return gDriver ? gDriver->currentBss(out) : -1;
}

extern "C" int switch_wlan_connect(const uint8_t *ssid, size_t ssid_len, const uint8_t *bssid, const uint8_t *key,
                                   size_t key_len, int key_is_pmk)
{
    return gDriver ? gDriver->requestJoin(ssid, ssid_len, bssid, key, key_len, key_is_pmk != 0) : -1;
}

extern "C" void switch_wlan_disconnect(void)
{
    if (gDriver)
        gDriver->requestLeave();
}

extern "C" int switch_wlan_power(int on)
{
    return gDriver ? gDriver->requestPower(on != 0) : -1;
}

extern "C" int switch_wlan_tx(const uint8_t *eth, size_t length)
{
    return gDriver ? gDriver->transmit(eth, length) : -1;
}

extern "C" unsigned switch_wlan_tx_room(void)
{
    return gDriver ? gDriver->txRoom() : 0;
}

extern "C" unsigned switch_wlan_tx_frames(swlan_tx_fill_fn fill, void *context, unsigned max)
{
    return gDriver ? gDriver->transmitFrames(fill, context, max) : 0;
}

extern "C" void switch_wlan_status_get(struct swlan_status *out)
{
    if (gDriver) {
        gDriver->status(out);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

extern "C" void switch_wlan_bt_links(unsigned links)
{
    if (gDriver)
        gDriver->btLinks(links);
}

extern "C" void switch_wlan_bt_state(uint32_t flags, int power)
{
    if (gDriver)
        gDriver->btState(flags, power != 0);
}

extern "C" void switch_wlan_multicast(const uint8_t (*groups)[6], unsigned count, int all)
{
    if (gDriver)
        gDriver->multicast(groups, count, all != 0);
}

extern "C" void switch_wlan_counters(char *text, size_t size)
{
    if (gDriver) {
        gDriver->counters(text, size);
    } else if (size) {
        text[0] = 0;
    }
}

// SwitchWLANAwdlRadio.h §6, the firmware's own AWDL; the soft backend's calls (§2-§5) are SwitchWLANAwdlSoft.cpp's.
extern "C" int swlan_awdl_open(const struct swlan_awdl_caps **caps_out)
{
    if (caps_out)
        *caps_out = nullptr;
    return gDriver ? gDriver->awdlOpen(caps_out) : -ENXIO;
}

extern "C" int swlan_awdl_query(const struct swlan_awdl_caps **caps_out)
{
    if (caps_out)
        *caps_out = nullptr;
    return gDriver ? gDriver->awdlQuery(caps_out) : -EAGAIN;
}

extern "C" void swlan_awdl_close(void)
{
    if (gDriver)
        gDriver->awdlClose();
}

extern "C" int swlan_awdl_var(int set, uint8_t ifidx, const char *name, const void *in, size_t in_len, void *out,
                              size_t out_len, size_t *returned)
{
    return gDriver ? gDriver->awdlVar(set != 0, ifidx, name, in, in_len, out, out_len, returned) : -ENXIO;
}

extern "C" int swlan_awdl_fw_events(swlan_fw_event_fn fn, void *ctx)
{
    if (!gDriver || !fn)
        return -ENXIO;
    gDriver->awdlCallbacks(fn, ctx, nullptr, nullptr, nullptr, nullptr);
    return 0;
}

extern "C" int swlan_awdl_eth_rx(swlan_eth_rx_fn fn, void *ctx)
{
    if (!gDriver || !fn)
        return -ENXIO;
    gDriver->awdlCallbacks(nullptr, nullptr, fn, ctx, nullptr, nullptr);
    return 0;
}

extern "C" int swlan_awdl_events(swlan_evt_fn fn, void *ctx)
{
    if (!gDriver || !fn)
        return -ENXIO;
    gDriver->awdlCallbacks(nullptr, nullptr, nullptr, nullptr, fn, ctx);
    return 0;
}

extern "C" int swlan_awdl_eth_tx(const uint8_t *eth, size_t len)
{
    return gDriver ? gDriver->awdlTransmit(eth, len) : -ENXIO;
}

extern "C" int switch_wlan_ioctl_set(uint32_t cmd, const void *buf, size_t length)
{
    return gDriver ? gDriver->awdlIoctlSet(cmd, buf, length) : -ENXIO;
}

extern "C" int switch_wlan_d11n(void)
{
    return gDriver ? gDriver->awdlD11n() : 0;
}

// The radio's address where the system reads it, before userland starts: /chosen mac-address-wifi0 (MobileGestalt's
// WifiAddress; wifid derives every network's private address from it and aborts a join without it, UniXNU-sheng
// WifiProbe.cpp) and /arm-io/wlan local-mac-address (as J120's device tree has it). The packager's placeholders are
// replaced with the fuse address the chip is given.
static void setBytes(IORegistryEntry *entry, const char *key, const uint8_t *bytes, unsigned length)
{
    if (OSData *data = OSData::withBytes(bytes, length)) {
        entry->setProperty(key, data);
        data->release();
    }
}

static void publishMac(const uint8_t mac[6])
{
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        setBytes(chosen, "mac-address-wifi0", mac, 6);
        chosen->release();
    }
    if (IORegistryEntry *wlan = IORegistryEntry::fromPath("/arm-io/wlan", gIODTPlane)) {
        setBytes(wlan, "local-mac-address", mac, 6);
        wlan->release();
    }
}

// Called once from the kernel's boot path (SwitchRootDiag.h, after the SD block device): the address in the device
// tree, then the driver on IOResources unless switch-no-wlan. Returns 0 when off or started. (The bring-up register
// report, switch_wlan_probe, is no longer printed at boot.)
extern "C" int switch_wlan_publish(void)
{
    uint32_t off = 0;
    if (PE_parse_boot_argn("switch-no-wlan", &off, sizeof(off)) && off)
        return 0;
    if (gDriver)
        return 0;
    uint8_t mac[6];
    if (switch_wlan_mac(mac, nullptr)) {
        // The fuses were not readable (switch_wlan_mac needs the fuse clock and FUSE_VISIBLE): a locally administered
        // address, as nxk uses, rather than the NVRAM's placeholder shared by every console.
        static const uint8_t fallback[6] = {0x02, 0x4E, 0x58, 0x4B, 0x57, 0x01};
        memcpy(mac, fallback, sizeof(mac));
        IOLog("SwitchWLAN: fuses not readable, using the locally administered address 02:4e:58:4b:57:01\n");
    }
    publishMac(mac);
    // Without a platform UUID (made from /chosen unique-chip-id and chip-id) gethostuuid() waits for ever, and configd's
    // IPConfiguration start() calls it: configd never lists en0 and wifid never makes a Wi-Fi device.
    const IOService *platform = IOService::getPlatform();
    if (!platform || !platform->getProvider() || !platform->getProvider()->getProperty(kIOPlatformUUIDKey)) {
        IOLog("SwitchWLAN: no IOPlatformUUID (ADT /chosen unique-chip-id, chip-id): configd will stall, no Wi-Fi in "
              "Settings\n");
    }
    IOService *resources = IOService::getResourceService();
    SwitchWLAN *candidate = OSTypeAlloc(SwitchWLAN);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources)) {
        IOLog("SwitchWLAN: attach failed\n");
        OSSafeReleaseNULL(candidate);
        return -1;
    }
    candidate->setMac(mac);
    gDriver = candidate;
    if (!candidate->start(resources)) {
        gDriver = nullptr;
        candidate->detach(resources);
        candidate->release();
        return -1;
    }
    return 0;
}

extern "C" void switch_wlan_suspend(void)
{
    if (gDriver)
        gDriver->systemSleep();
}

extern "C" void switch_wlan_resume(void)
{
    if (gDriver)
        gDriver->systemWake();
}
