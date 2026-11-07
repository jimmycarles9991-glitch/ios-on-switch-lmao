// SPDX-License-Identifier: BSD-2-Clause-Patent
// The network side's three parts and what they call of each other:
//   SwitchWLANFamily.cpp     the IONetworkingFamily classes (a late object: their metaclasses need the family loaded)
//   SwitchWLANNet.cpp        en0's data path between the radio's DMA buffers and the family's queues
//   SwitchWLANApple80211.cpp wifid's Apple80211 requests and the events that answer them
#pragma once

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <stdint.h>
#include <sys/kernel_types.h>

class IOEthernetController;
class IOEthernetInterface;

namespace swlan::net {

// SwitchWLANFamily.cpp. Loads IONetworkingFamily, registers the classes, starts the controller below `provider` on
// `loop` (the radio's work loop: received frames come from it) and attaches its interface, which IONetworkStack names
// and attaches to the stack. False (one line logged) when any of that fails.
bool publish(IOService *provider, IOWorkLoop *loop, const uint8_t mac[6]);

// SwitchWLANNet.cpp, called by the family classes.
void attached(IOEthernetController *controller, IOEthernetInterface *interface);
IOReturn output(IOEthernetInterface *interface);
void multicast(const uint8_t (*groups)[6], unsigned count, bool all);
void multicast_all(bool all);

// SwitchWLANNet.cpp, called by SwitchWLANApple80211.cpp: the link as the controller reports it (IOLinkStatus and the
// stack's link events), and the frames queued for a network that went.
void set_link(bool up);
void purge();
// en0's ifnet once IONetworkStack attached it, else NULL.
ifnet_t ifnet();

// SwitchWLANApple80211.cpp: an Apple80211 request (SIOCGA80211/SIOCSA80211 on en0), the 802.11 media
// (SIOCGIFMEDIA), and the radio's events.
int apple80211(bool set, void *request);
int media(void *request);
void scan_done();
void associated();
void connect_done(int result);
void link_lost(int reason);

}  // namespace swlan::net

// Called once the radio is up with its address (SwitchWLAN.cpp, worker thread): en0 below `radio`, on its work loop.
void switch_wlan_net_start(IOService *radio, IOWorkLoop *loop, const uint8_t mac[6]);
