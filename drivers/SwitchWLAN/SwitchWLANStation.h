// SPDX-License-Identifier: BSD-2-Clause-Patent
// The boundary between the Switch's Wi-Fi radio (SwitchWLAN.cpp: Tegra PCIe, BCM4356A3 FullMAC firmware over
// msgbuf rings, the WPA2 supplicant) and the network side (SwitchWLANNet.h: en0 on IONetworkingFamily and the
// Apple80211 requests wifid sends). Its shape is UniXNU-sheng's nw_abi.h station half (the WifiNet.cpp
// this network side is ported from calls exactly these), with the key passed as what wifid hands over.
// Plain C types only.
#ifndef SWITCH_WLAN_STATION_H
#define SWITCH_WLAN_STATION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SWLAN_STA_OFF,
    SWLAN_STA_IDLE,
    SWLAN_STA_SCANNING,
    SWLAN_STA_AUTH,
    SWLAN_STA_ASSOC,
    SWLAN_STA_KEYS,
    SWLAN_STA_UP
};

enum {
    SWLAN_RES_OK,
    SWLAN_RES_NOT_FOUND,
    SWLAN_RES_TIMEOUT,
    SWLAN_RES_REJECTED,
    SWLAN_RES_BAD_KEY,
    SWLAN_RES_UNSUPPORTED,
    SWLAN_RES_FAILED,
};

struct swlan_bss {
    uint8_t bssid[6];
    uint8_t ssid_len;
    uint8_t ssid[32];
    uint32_t freq_mhz;
    int8_t signal_dbm;
    uint16_t caps, beacon_int;
    uint32_t age_ms;
    uint16_t ie_len;
    uint8_t ie[384];  // information elements as the access point sent them
    uint16_t bw_mhz;  // the width it runs (its chanspec): 20, 40, 80, 160; 0 when not known
    uint8_t center;   // the channel that width is centred on
};

struct swlan_status {
    uint32_t state;  // SWLAN_STA_*
    int32_t result;  // of the last join, SWLAN_RES_*
    uint8_t bssid[6];
    uint8_t ssid_len;
    uint8_t ssid[32];
    uint32_t freq_mhz;
    int8_t signal_dbm;
    uint8_t mac[6];
    uint32_t power;   // radio on
    uint16_t bw_mhz;  // the joined access point's width and centre channel (swlan_bss)
    uint8_t center;
    uint32_t tx_rate_kbps, rx_rate_kbps;  // the link's PHY rates as the firmware last reported them (0: not yet)
};

// Called from the radio's threads: scan_done, associated, connect_done and link_lost from its worker thread, rx from
// its work loop. Handlers must not block. A join that gets as far as the access point calls associated (the 802.11
// link is up, the key handshake of a secured network still to come) and then connect_done with how it ended.
struct swlan_events {
    void (*scan_done)();
    void (*associated)();
    void (*connect_done)(int result);  // SWLAN_RES_*
    void (*link_lost)(int reason);
    void (*rx)(const uint8_t *eth, size_t length);  // Ethernet header + payload
    // From the work loop too: after each interrupt's frames (rx_flush: hand what rx gathered to the stack in one go),
    // and when the radio can take frames again after switch_wlan_tx_room said 0, or the station came up (tx_ready).
    void (*rx_flush)();
    void (*tx_ready)();
};

// Radio side. Every call only queues work (or copies a snapshot) and returns; any thread context.
void switch_wlan_set_events(const struct swlan_events *events);
int switch_wlan_scan(const uint8_t *ssid, size_t ssid_len);  // ssid NULL: every network
int switch_wlan_bss_count(void);
int switch_wlan_bss_get(int index, struct swlan_bss *out);  // 0 on success
// The access point the radio is associated with, as the firmware reports it (it picks the access point itself, so
// it need not be in the scan table). 0 while associated.
int switch_wlan_current_bss(struct swlan_bss *out);
// key NULL: open network. key_is_pmk: the 32-byte PMK wifid derived (Apple80211 cipher 6); otherwise a passphrase
// of 8..63 characters or the PMK as 64 hex digits. bssid NULL: any access point of that name.
int switch_wlan_connect(const uint8_t *ssid, size_t ssid_len, const uint8_t *bssid, const uint8_t *key, size_t key_len,
                        int key_is_pmk);
void switch_wlan_disconnect(void);
int switch_wlan_power(int on);
int switch_wlan_tx(const uint8_t *eth, size_t length);  // 0 queued (one frame, copied)
// en0's output, from its start routine (thread context): how many frames the radio takes now. 0 while the station
// is not up or every transmit buffer is in flight; tx_ready follows once that changes.
unsigned switch_wlan_tx_room(void);
// Up to `max` frames, each written by `fill` straight into the DMA buffer it goes out of (fill returns the frame's
// length, at least 14 and at most `room` bytes, or 0 for no more), then one doorbell for them all. Frames taken.
typedef size_t (*swlan_tx_fill_fn)(void *context, uint8_t *buffer, size_t room);
unsigned switch_wlan_tx_frames(swlan_tx_fill_fn fill, void *context, unsigned max);
void switch_wlan_status_get(struct swlan_status *out);
void switch_wlan_counters(char *text, size_t size);  // frames and losses, one line
// What wifid says Bluetooth is doing (APPLE80211_IOC_BT_COEX_FLAGS: SWLAN_BT_* of SwitchWLANCoex.h) and whether its
// power is on (APPLE80211_IOC_BT_POWER), for the radio's coexistence handling and its stats line.
void switch_wlan_bt_state(uint32_t flags, int power);
// SwitchBluetooth: how many links (ACL and LE) the controller has, at every change; the coexistence mode follows it.
void switch_wlan_bt_links(unsigned links);
// en0's multicast filter, at every change: the link-layer groups the stack joined and whether it wants every group
// (IFF_ALLMULTI, or more groups than the firmware's list holds). Programmed now and after every bring-up.
#define SWLAN_MCAST_MAX 32
void switch_wlan_multicast(const uint8_t (*groups)[6], unsigned count, int all);

// SwitchWLANNet.cpp: what en0 did with the frames, for the radio's stats line.
struct swlan_net_counters {
    uint32_t rx_input, rx_no_mbuf, rx_input_errors;  // onto the input queue, no mbuf for them, refused by the family
    uint32_t tx_oversize;                            // dequeued but larger than a transmit buffer
    uint32_t tx_starts, tx_batches;                  // outputStart runs, and the batches it handed to the radio
    uint32_t rx_batches;                             // input queue flushes (one ifnet input each)
};

void switch_wlan_net_counters(struct swlan_net_counters *out);
// The link's transmit PHY rate (b/s) as en0's baud rate, what the stack and its clients read for the link speed.
void switch_wlan_net_rate(uint64_t bps);

// SwitchWLAN.cpp: the radio driver (publishes the fuse MAC in the device tree, then brings the chip up on its own
// thread). Returns 0 when boot-arg switch-no-wlan is set or the driver started. Thread context, IOKit up, before userland.
int switch_wlan_publish(void);

// SwitchWLANProbe.cpp
void switch_wlan_probe(void);                         // read-only state report
int switch_wlan_mac(uint8_t wifi[6], uint8_t bt[6]);  // 0: fuse-derived addresses as switchroot U-Boot makes them
// System sleep (drivers/SwitchSuspend's sleep and wake actions, one thread): suspend lets the chip go and parks the
// worker (bounded at 3 s); resume brings PCIe and the chip up whole on the worker, back on the last network.
void switch_wlan_suspend(void);
void switch_wlan_resume(void);

#ifdef __cplusplus
}
#endif

#endif
