// SPDX-License-Identifier: BSD-2-Clause-Patent
// wifid's side of the Wi-Fi radio: the Apple80211 requests (ioctls on en0) it uses to scan, join and read state, and
// the Apple80211 events that tell it what the radio did. Nothing here talks to the radio: it calls the station
// interface (SwitchWLANStation.h) and turns what it answers into the structures the system's wireless framework
// reads. DHCP and everything above the link are iOS's own (configd, IPConfiguration).
//
// Ported from UniXNU-sheng package/WifiNet.cpp (BSD-2-Clause-Patent; iPadOS 17.7.11 21H461, the same build as ours,
// where wifid scans and joins WPA2 networks through it): request numbers and events are sheng's. Dropped: its
// unattended join.txt test join, the traffic and input-stall diagnostics (they use sheng's boot tracing). Changed: the
// key goes to our supplicant as wifid gives it (a 32-byte PMK for cipher 6, else the passphrase).
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <net/kpi_interface.h>
#include <sys/errno.h>
#include <sys/kern_event.h>
#include <sys/systm.h>
#include "SwitchWLANApple80211.h"
#include "SwitchWLANAwdlRadio.h"
#include "SwitchWLANNet.h"
#include "SwitchWLANStation.h"

// (the header declares it without C linkage; the symbol is the C one)
extern "C" int swlan_kev_post(struct kev_msg *) __asm__("_kev_post_msg");

namespace {

using namespace swlan::a80211;

bool gPower = true;
bool gKeyed;            // the last join used a key
bool gLinkAnnounced;    // wifid was told the link is up (an association) and not yet that it went down
uint32_t gBtFlags;      // wifid's APPLE80211_IOC_BT_COEX_FLAGS: SWLAN_BT_* (SwitchWLANCoex.h)
uint32_t gBtPower = 1;  // wifid's APPLE80211_IOC_BT_POWER
int gAssocResult = kAssocOk;
unsigned gScanCursor, gScanning;

// Events to the framework: class IEEE 802.11 (6), subclass 1, data starting with the interface's name. None before
// IONetworkStack attached en0: nothing has asked about it yet.
void post_event(uint32_t code, const void *payload, size_t length)
{
    ifnet_t ifp = swlan::net::ifnet();
    if (!ifp)
        return;
    uint8_t data[128];
    if (length > sizeof data - 16)
        length = sizeof data - 16;
    memset(data, 0, 16);
    snprintf(reinterpret_cast<char *>(data), 16, "%s%u", ifnet_name(ifp), ifnet_unit(ifp));
    if (length)
        memcpy(data + 16, payload, length);
    struct kev_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.vendor_code = KEV_VENDOR_APPLE;
    msg.kev_class = 6;
    msg.kev_subclass = 1;
    msg.event_code = code;
    msg.dv[0].data_length = static_cast<uint32_t>(16 + length);
    msg.dv[0].data_ptr = data;
    swlan_kev_post(&msg);
}

void on_scan_done()
{
    __atomic_store_n(&gScanning, 0u, __ATOMIC_RELEASE);
    gScanCursor = 0;
    post_event(kEvScanDone, nullptr, 0);
}

// A scan answered from the cache finishes after the time a short scan takes: answered at once, the daemon asks again
// straight away.
thread_call_t gCachedScan;

void cached_scan_done(thread_call_param_t, thread_call_param_t)
{
    on_scan_done();
}

// A link down carries the last signal too: wifid keeps it as the network's RSSI at the loss and lets the network
// auto-join again once a scan sees it better than that (__WiFiDeviceManagerHasNetworkRssiImproved; 0 is "invalid
// blacklistedRssi").
void post_link_event(bool up, int reason)
{
    link_event ev;
    memset(&ev, 0, sizeof ev);
    swlan_status st;
    switch_wlan_status_get(&st);
    ev.rssi = st.signal_dbm;
    ev.noise = static_cast<uint16_t>(-95);
    ev.snr = static_cast<uint16_t>(st.signal_dbm + 95);
    if (!up) {
        ev.down = 1;
        ev.involuntary = 1;
        ev.reason = static_cast<uint32_t>(reason);
    }
    gLinkAnnounced = up;
    post_event(kEvLinkChanged, &ev, sizeof ev);
}

// APPLE80211_C_FLAG_*: 20 MHz 0x2, 40 MHz 0x4 (0x200: its extension channel above), 80 MHz 0x400, 160 MHz 0x800;
// 2.4 GHz 0x8, 5 GHz 0x10; active scanning 0x80; DFS 0x100.
void fill_channel(channel &c, uint32_t mhz, uint16_t bw = 20, uint8_t center = 0)
{
    c.version = 1;
    if (mhz >= 4900) {
        c.channel = (mhz - 5000) / 5;
        c.flags = 0x10;
    } else {
        c.channel = mhz == 2484 ? 14 : (mhz - 2407) / 5;
        c.flags = 0x8;
    }
    if (bw >= 160)
        c.flags |= 0x800;
    else if (bw >= 80)
        c.flags |= 0x400;
    else if (bw >= 40)
        c.flags |= 0x4 | (center > c.channel ? 0x200 : 0);
    else
        c.flags |= 0x2;
}

void fill_result(scan_result &r, const swlan_bss &b)
{
    memset(&r, 0, sizeof r);
    r.version = 1;
    fill_channel(r.ch, b.freq_mhz, b.bw_mhz, b.center);
    r.noise_known = 1;
    r.noise = -95;
    r.rssi = b.signal_dbm;
    r.snr = static_cast<int16_t>(b.signal_dbm + 95);
    r.beacon_interval = b.beacon_int;
    r.capability = b.caps;
    memcpy(r.bssid, b.bssid, 6);
    r.ssid_length = b.ssid_len;
    memcpy(r.ssid, b.ssid, 32);
    r.age_ms = b.age_ms;
    r.elements_length = b.ie_len > sizeof r.elements ? sizeof r.elements : b.ie_len;
    memcpy(r.elements, b.ie, r.elements_length);
}

int copy_out(const apple80211req &rq, const void *src, size_t size)
{
    if (rq.req_len == 0 || !rq.req_data)
        return EINVAL;
    return copyout(src, rq.req_data, rq.req_len < size ? rq.req_len : size);
}

// A set's single value: in req_val without a buffer, else the buffer's {version, value}.
uint32_t set_value(const apple80211req &rq)
{
    uint32_t w[2] = {};
    if (rq.req_len >= sizeof(w) && rq.req_data && !copyin(rq.req_data, w, sizeof(w)))
        return w[1];
    return static_cast<uint32_t>(rq.req_val);
}

// A get that gives no buffer takes a single value in req_val; one with a buffer gets {version, value}.
int answer_value(apple80211req &rq, uint32_t value)
{
    if (rq.req_len == 0 || !rq.req_data) {
        rq.req_val = static_cast<int32_t>(value);
        return 0;
    }
    const uint32_t w[2] = {1, value};
    return copyout(w, rq.req_data, rq.req_len < sizeof w ? rq.req_len : sizeof w);
}

// The raw bytes, their length in req_len.
int answer_bytes(apple80211req &rq, const void *src, uint32_t size)
{
    if (!rq.req_data)
        return EINVAL;
    const uint32_t n = rq.req_len < size ? rq.req_len : size;
    rq.req_len = size;
    return copyout(src, rq.req_data, n);
}

int copy_in(const apple80211req &rq, void *dst, size_t size)
{
    if (!rq.req_data)
        return EINVAL;
    memset(dst, 0, size);
    return copyin(rq.req_data, dst, rq.req_len < size ? rq.req_len : size);
}

// The regulatory country as the access points announce it (802.11d: the Country element, id 7, of the strongest
// network that has one), else "XX".
void country(char cc[3])
{
    cc[0] = 'X';
    cc[1] = 'X';
    cc[2] = 0;
    int best = -200;
    swlan_bss *b = static_cast<swlan_bss *>(IOMallocData(sizeof(swlan_bss)));
    if (!b)
        return;
    for (int i = 0, n = switch_wlan_bss_count(); i < n; ++i) {
        if (switch_wlan_bss_get(i, b) || b->signal_dbm <= best)
            continue;
        for (unsigned o = 0; o + 2 <= b->ie_len && o + 2 + b->ie[o + 1] <= b->ie_len; o += 2 + b->ie[o + 1])
            if (b->ie[o] == 7 && b->ie[o + 1] >= 2 && b->ie[o + 2] >= 'A' && b->ie[o + 2] <= 'Z' &&
                b->ie[o + 3] >= 'A' && b->ie[o + 3] <= 'Z') {
                cc[0] = static_cast<char>(b->ie[o + 2]);
                cc[1] = static_cast<char>(b->ie[o + 3]);
                best = b->signal_dbm;
                break;
            }
    }
    IOFreeData(b, sizeof(swlan_bss));
}

// APPLE80211_S_*: init, scan, auth, assoc, run. As in net80211, run starts at the association (the link up wifid is
// given then); the key handshake happens in it.
int map_state(unsigned s)
{
    switch (s) {
    case SWLAN_STA_SCANNING:
        return 1;
    case SWLAN_STA_AUTH:
        return 2;
    case SWLAN_STA_ASSOC:
        return 3;
    case SWLAN_STA_KEYS:
    case SWLAN_STA_UP:
        return 4;
    default:
        return 0;
    }
}

int answer(bool set, apple80211req &rq)
{
    swlan_status st;
    switch_wlan_status_get(&st);
    const int type = rq.req_type;

    // (not on the stack: the largest answer is 4824 bytes, plus a scan record to fill)
    union Out {
        uint8_t raw[5120];
        channels_list chans;
        scan_result res;
    };

    struct Scratch {
        Out out;
        swlan_bss bss;
    };

    Scratch *scratch = static_cast<Scratch *>(IOMallocData(sizeof(Scratch)));
    if (!scratch)
        return ENOMEM;
    memset(scratch, 0, sizeof(Scratch));

    struct Free {
        Scratch *p;

        ~Free()
        {
            IOFreeData(p, sizeof(Scratch));
        }
    } freeScratch{scratch};

    Out &out = scratch->out;
    swlan_bss &b = scratch->bss;
    uint32_t *w = reinterpret_cast<uint32_t *>(out.raw);
    if (set) {
        switch (type) {
        case kPower: {
            uint32_t in[6];
            if (copy_in(rq, in, sizeof in))
                return EFAULT;
            gPower = in[2] != 0;
            switch_wlan_power(gPower);
            if (!gPower)
                swlan::net::set_link(false);
            post_event(kEvPowerChanged, nullptr, 0);
            return 0;
        }
        case kScanReq:
        case kScanReqMultiple: {
            uint8_t buf[0x80];
            if (copy_in(rq, buf, sizeof buf))
                return EFAULT;
            uint32_t ssidLength;
            memcpy(&ssidLength, buf + 16, 4);
            const uint8_t *ssid = buf + 20;
            // Joining or joined: a scan takes the radio off channel for seconds and the access point's beacons and
            // handshake frames are missed, so the daemon gets the last results instead.
            if (st.state >= SWLAN_STA_AUTH && switch_wlan_bss_count() > 0) {
                if (!gCachedScan)
                    gCachedScan = thread_call_allocate(cached_scan_done, nullptr);
                gScanning = 1;
                gScanCursor = 0;
                uint64_t at;
                clock_interval_to_deadline(2000, kMillisecondScale, &at);
                thread_call_enter_delayed(gCachedScan, at);
                return 0;
            }
            gScanning = 1;
            gScanCursor = 0;
            return switch_wlan_scan(ssidLength && ssidLength <= 32 ? ssid : nullptr, ssidLength <= 32 ? ssidLength : 0)
                       ? EBUSY
                       : 0;
        }
        case kAssociate: {
            assoc_request a;
            if (copy_in(rq, &a, sizeof a))
                return EFAULT;
            if (a.ssid_length > 32 || a.key_length > 64)
                return EINVAL;
            const bool hasKey = a.key_length != 0;
            // cipher 6: the daemon already derived the pairwise master key (32 bytes), which the supplicant takes as is
            const bool pmk = hasKey && a.key_cipher == 6 && a.key_length == 32;
            gAssocResult = kAssocFailed;
            gKeyed = hasKey;
            IOLog("SwitchWLAN: wifid asks to join a %u-byte name, %s\n", a.ssid_length,
                  !hasKey ? "open"
                  : pmk   ? "with its PMK"
                          : "with a passphrase");
            const int r = switch_wlan_connect(a.ssid, a.ssid_length, a.bssid, hasKey ? a.key : nullptr,
                                              hasKey ? a.key_length : 0, pmk);
            memset(&a, 0, sizeof a);
            return r ? EINVAL : 0;
        }
        case kDisassociate:
        case kDeauth:
            switch_wlan_disconnect();
            swlan::net::set_link(false);
            swlan::net::purge();
            gLinkAnnounced = false;  // wifid's own leaving: it knows
            return 0;
        case kVirtualIfCreate:
        case kVirtualIfDelete: {
            uint8_t vif[32];
            if (copy_in(rq, vif, sizeof vif))
                return EFAULT;
            const int r = type == kVirtualIfCreate ? switch_wlan_awdl_vif_create(vif, sizeof vif)
                                                   : switch_wlan_awdl_vif_delete(vif, sizeof vif);
            // the created interface's name goes back in the caller's buffer
            if (!r && type == kVirtualIfCreate &&
                copyout(vif, rq.req_data, rq.req_len < sizeof vif ? rq.req_len : sizeof vif))
                return EFAULT;
            return r;
        }
        case kBtCoexFlags:
        case kBtPower:
            // What Bluetooth does, for the radio's coexistence handling and its stats line (SwitchWLANCoex.h); wifid
            // logged "APPLE80211_IOC_BT_COEX_FLAGS return -3903" at every A2DP start and stop while this was refused.
            if (type == kBtCoexFlags)
                gBtFlags = set_value(rq);
            else
                gBtPower = set_value(rq) != 0;
            switch_wlan_bt_state(gBtFlags, gBtPower != 0);
            return 0;
        case kScanCacheClear:
        case kLocale:
        case kCountryCode:
        case kPowerSave:
        case kAuthType:
        case kCipherKey:
            return 0;
        default:
            return EOPNOTSUPP;
        }
    }
    switch (type) {
    case kPower:
        w[0] = 1;
        w[1] = 1;
        w[2] = gPower ? 1 : 0;
        return copy_out(rq, w, 24);
    case kState:
        return answer_value(rq, static_cast<uint32_t>(gPower ? map_state(st.state) : 0));
    case kOpMode:
        return answer_value(rq, 1);  // station
    case kAuthType:
        w[0] = 1;
        w[1] = 1;
        w[2] = gKeyed ? 0x8 : 0;
        return copy_out(rq, w, 12);  // {version, open system, WPA2 personal or none}
    case kPowerSave:
        return answer_value(rq, 0);
    case kCardCaps: {  // a bitmap, bit n in byte n / 8: AES, AES-CCM, power management, short slot and preamble, WPA2, WME, short guard interval at 20 MHz
        static const uint8_t has[] = {2, 3, 6, 9, 10, 14, 17, 19};
        for (uint8_t bit : has)
            out.raw[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
        // AWDL (bit 32, what wifip2pd gates awdl0 on) when the radio's probe found the firmware's AWDL (SwitchWLANAwdl.cpp)
        if (switch_wlan_awdl_capable() == 1)
            out.raw[4] |= 1;
        return answer_bytes(rq, out.raw, 12);
    }
    case kPhyMode:
        w[0] = 1;
        w[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40;
        w[2] = st.state >= SWLAN_STA_ASSOC ? 0x8 : 0;
        return copy_out(rq, w, 12);
    case kSsid:
        if (st.state < SWLAN_STA_ASSOC)
            return ENOENT;
        return answer_bytes(rq, st.ssid, st.ssid_len);
    case kBssid:
        if (st.state < SWLAN_STA_ASSOC)
            return ENOENT;
        return answer_bytes(rq, st.bssid, 6);
    case kChannel:
        if (st.state < SWLAN_STA_AUTH)
            return ENOENT;
        w[0] = 1;
        fill_channel(*reinterpret_cast<channel *>(out.raw + 4), st.freq_mhz, st.bw_mhz, st.center);
        return copy_out(rq, w, 16);
    case kRssi:
    case kNoise: {  // {version, radios, unit (0: dBm), per chain[4], aggregate, extension per chain[4], its aggregate}
        if (type == kRssi && st.state < SWLAN_STA_ASSOC)
            return ENOENT;
        const uint32_t v = static_cast<uint32_t>(type == kRssi ? static_cast<int32_t>(st.signal_dbm) : -95);
        w[0] = 1;
        w[1] = 1;
        w[2] = 0;
        for (int i = 3; i < 13; i++)
            w[i] = v;
        return copy_out(rq, w, 13 * 4);
    }
    case kRate:  // Mb/s: the firmware's transmit rate once known, 54 until then
        return answer_value(rq, st.state < SWLAN_STA_ASSOC ? 0 : st.tx_rate_kbps ? st.tx_rate_kbps / 1000 : 54);
    case kTxPower:
        return answer_value(rq, 15);
    case kDriverVersion:
    case kHardwareVersion: {
        strlcpy(reinterpret_cast<char *>(out.raw), type == kDriverVersion ? "SwitchWLAN 1" : "BCM4356A3", 256);
        return copy_out(rq, out.raw, 256);
    }
    case kCountryCode: {
        char cc[3];
        country(cc);
        return answer_bytes(rq, cc, 3);
    }  // two letters and a NUL
    case kAssociateResult:
        return answer_value(rq, static_cast<uint32_t>(gAssocResult));
    case kBtCoexFlags:
        return answer_value(rq, gBtFlags);
    case kBtPower:
        return answer_value(rq, gBtPower);
    case kSupportedChannels:
    case kHwSupportedChannels: {
        out.chans.version = 1;
        unsigned n = 0;
        // The firmware's regulatory domain (US/0): 2.4 GHz 1-11, 5 GHz 36-48 and 149-165 scanned actively, and the
        // DFS channels 52-64 and 100-144, scanned passively (its escan covers them; wifid listed a network on 52).
        static const uint16_t known[] = {2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 5180,
                                         5200, 5220, 5240, 5745, 5765, 5785, 5805, 5825, 5260, 5280, 5300, 5320,
                                         5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700, 5720};
        for (uint16_t f : known) {
            fill_channel(*reinterpret_cast<channel *>(&out.chans.list[n]), f);
            const bool dfs = f >= 5250 && f <= 5730;
            out.chans.list[n].flags |= dfs ? 0x100 : 0x80;
            n++;
        }
        out.chans.count = n;
        return copy_out(rq, &out.chans, sizeof out.chans);
    }
    case kScanResult: {
        const int count = switch_wlan_bss_count();
        if (gScanCursor >= static_cast<unsigned>(count) || switch_wlan_bss_get(static_cast<int>(gScanCursor), &b)) {
            gScanCursor = 0;
            return EIO;  // after the last record
        }
        gScanCursor++;
        fill_result(out.res, b);
        return copy_out(rq, &out.res, sizeof out.res);
    }
    case kRssiBounds:
        // {version, minimum, maximum, threshold} dBm (IO80211.framework 21H461 makes RSSI_MIN, RSSI_MAX, RSSI_THRESHOLD
        // of words 1-3). wifid reads it once, when it makes the Wi-Fi device, and auto-joins only networks above the
        // threshold: unanswered it is 0 and every network is "Below minimum RSSI threshold" (r3j, r3v, r4a).
        w[0] = 1;
        w[1] = static_cast<uint32_t>(-100);
        w[2] = 0;
        w[3] = static_cast<uint32_t>(-80);  // wifid's own bad-RSSI line (WiFiUsagePoorLinkSession badRssiThreshold)
        return copy_out(rq, w, 16);
    case kCurrentNetwork:  // the access point the firmware joined, which need not be in the scan table
        if (switch_wlan_current_bss(&b))
            return ENOENT;
        fill_result(out.res, b);
        return copy_out(rq, &out.res, sizeof out.res);
    default:
        return EOPNOTSUPP;
    }
}

// SIOCGIFMEDIA: IO80211.framework lists wireless interfaces by the 802.11 media type, configd reads the link state
// from the status. The family's own answer is its medium dictionary's, which has no 802.11 media.
int answer_media(struct ifmediareq64 &m)
{
    swlan_status st;
    switch_wlan_status_get(&st);
    static const int media = IFM_IEEE80211 | IFM_AUTO;
    m.ifm_current = media;
    m.ifm_mask = 0;
    m.ifm_status = IFM_AVALID | (st.state == SWLAN_STA_UP ? IFM_ACTIVE : 0);
    m.ifm_active = media;
    if (m.ifmu_ulist && m.ifm_count > 0 && copyout(&media, m.ifmu_ulist, sizeof media))
        return EFAULT;
    m.ifm_count = 1;
    return 0;
}

}  // namespace

namespace swlan::net {

int apple80211(bool set, void *request)
{
    return answer(set, *static_cast<apple80211req *>(request));
}

int media(void *request)
{
    return answer_media(*static_cast<struct ifmediareq64 *>(request));
}

void scan_done()
{
    on_scan_done();
}

// The order wifid (21H461) needs, as an iPad's own driver gives it: the link up at the association, the join's
// completion after the key handshake. wifid finds the joined network for a link up (APPLE80211_IOC_CURRENT_NETWORK;
// none: "Link up event with no current network detected. Dropping."), ranks en0 for routes (SetInterfaceRank
// Default) and, for a secured network, holds the link up from its clients ("Hold off dispatching client link up
// event until 4 way completes"). The association's completion (ASSOC_DONE, then __WiFiManagerJoinDoneCallback)
// hands the held link up to them; one that completes first finds nothing held, and Settings never shows the network
// joined (r3j: completion, then the link up, held for good).
void associated()
{
    post_event(kEvSsidChanged, nullptr, 0);
    post_event(kEvBssidChanged, nullptr, 0);
    post_link_event(true, 0);
}

void connect_done(int result)
{
    gAssocResult = result == SWLAN_RES_OK        ? kAssocOk
                   : result == SWLAN_RES_BAD_KEY ? kAssocBadKey
                   : result == SWLAN_RES_TIMEOUT ? kAssocTimeout
                                                 : kAssocFailed;
    if (result == SWLAN_RES_OK) {
        set_link(true);  // en0 carries data from here: the keys are in
        if (gKeyed) {
            // wifid's supplicant event: status 0, reason 0, the handshake done (its phase durations are not measured)
            supplicant_event sup;
            memset(&sup, 0, sizeof sup);
            post_event(kEvSupplicant, &sup, sizeof sup);
        }
    }
    post_event(kEvAssocDone, nullptr, 0);
    // A join that failed after the association (the key handshake): the link up wifid was given goes down again.
    if (result != SWLAN_RES_OK && gLinkAnnounced)
        post_link_event(false, 0);
}

void link_lost(int reason)
{
    set_link(false);
    post_link_event(false, reason);
    purge();
}

}  // namespace swlan::net
