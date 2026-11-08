// SPDX-License-Identifier: BSD-2-Clause-Patent
// WPA2-Personal supplicant (IEEE 802.11-2016 12.7.6): the 4-way handshake and the group key handshake, CCMP only.
// The console's firmware has none (Horizon runs its own): it hands received EAPOL frames up as E_EAPOL_MSG events,
// the replies go out on the TX flow ring, and the keys go back with the "wsec_key" iovar.
//
// Ported from MyEcoria/nxk user/wlan/src/wpa.rs (MIT OR Apache-2.0, joined real WPA2 networks on an Erista Switch).
// Plain C++ with no kernel dependency: the host test drives a full handshake through it.
#ifndef SWITCH_WLAN_WPA_H
#define SWITCH_WLAN_WPA_H

#include <stddef.h>
#include <stdint.h>

namespace swlan::wpa {

constexpr uint16_t kEthPae = 0x888E;
// Our RSN element for the association request: version 1, CCMP group and pairwise, PSK, no capabilities.
extern const uint8_t kRsnIe[22];

bool isEapol(const uint8_t *frame, size_t length);
// The information element `id` (header included) in a list of elements; nullptr if absent or truncated.
const uint8_t *element(const uint8_t *ies, size_t length, uint8_t id, size_t *elementLength);
// nullptr if the RSN element offers what this supplicant does (CCMP group and pairwise, PSK, no required MFP),
// else why not.
const char *checkRsn(const uint8_t *rsn, size_t length);

struct Gtk {
    uint8_t index;
    uint8_t key[16];
    uint8_t rsc[6];
};

// What one received EAPOL-Key frame asks for: a reply to send, a pairwise key and/or a group key to install.
struct Step {
    uint8_t reply[400];
    size_t replyLength;
    bool hasPtk, hasGtk;
    uint8_t tk[16];
    Gtk gtk;
};

class Supplicant {
public:
    using Random = void (*)(uint8_t *out, size_t length);

    void init(const uint8_t pmk[32], const uint8_t spa[6], Random random);
    void setIe(const uint8_t *ie, size_t length);  // the RSN element our association request carried
    void wipe();                                   // keys and nonces cleared

    bool hasAa() const
    {
        return haveAa_;
    }

    const uint8_t *aa() const
    {
        return aa_;
    }

    // Message 1 answered but no valid message 3: what a wrong passphrase looks like.
    bool unconfirmed() const
    {
        return haveTptk_ && !(havePtk_ && sameKey(ptk_, tptk_));
    }

    bool keyed() const
    {
        return havePtk_ && haveGtk_;
    }

    // nullptr when `step` holds what to do; else why the frame was dropped (nothing to do).
    const char *receive(const uint8_t *frame, size_t length, Step &step);

private:
    uint8_t pmk_[32], spa_[6], aa_[6], anonce_[32], snonce_[32], tptk_[48], ptk_[48], replay_[8];
    uint8_t ie_[64];
    size_t ieLength_ = 0;
    bool haveAa_ = false, haveTptk_ = false, havePtk_ = false, haveReplay_ = false, haveGtk_ = false;
    Gtk gtk_ = {};
    Random random_ = nullptr;

    static bool sameKey(const uint8_t *a, const uint8_t *b);
    struct Key;
    void derive(const uint8_t aa[6], uint8_t ptk[48]) const;
    void build(const Key &k, uint16_t info, const uint8_t nonce[32], const uint8_t *data, size_t dataLength,
               const uint8_t kck[16], Step &step) const;
    static const char *unwrapGtk(const Key &k, const uint8_t ptk[48], Gtk &gtk);
    bool fresh(const Gtk &gtk);
};

}  // namespace swlan::wpa

#endif
