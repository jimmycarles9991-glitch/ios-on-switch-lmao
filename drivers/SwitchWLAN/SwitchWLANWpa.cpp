// SPDX-License-Identifier: BSD-2-Clause-Patent
// See SwitchWLANWpa.h. Ported from MyEcoria/nxk user/wlan/src/wpa.rs (MIT OR Apache-2.0).
#include <string.h>
#include "SwitchWLANCrypto.h"
#include "SwitchWLANWpa.h"

namespace swlan::wpa {
namespace {

constexpr size_t kEthHeader = 14;
constexpr uint8_t kEapolKey = 3, kDescRsn = 2;
// Key information bits (802.11-2016 12.7.2).
constexpr unsigned kInfoVersion = 0x0007, kVersionAes = 2, kPairwise = 1u << 3, kInstall = 1u << 6, kAck = 1u << 7;
constexpr unsigned kMic = 1u << 8, kSecure = 1u << 9, kEncrypted = 1u << 12;
// EAPOL-Key frame layout from the start of the EAPOL header.
constexpr size_t kHeader = 4, kInfo = kHeader + 1, kReplay = kHeader + 5, kNonce = kHeader + 13, kRsc = kHeader + 61;
constexpr size_t kMicAt = kHeader + 77, kDataLength = kHeader + 93, kData = kHeader + 95, kMicLength = 16;
constexpr uint8_t kSuiteCcmp[4] = {0x00, 0x0F, 0xAC, 4}, kSuitePsk[4] = {0x00, 0x0F, 0xAC, 2};
constexpr uint16_t kRsnMfpRequired = 1u << 6;

inline uint16_t be16(const uint8_t *p)
{
    return static_cast<uint16_t>((static_cast<unsigned>(p[0]) << 8) | p[1]);
}

inline uint16_t le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}

// The MIC of an EAPOL frame: HMAC-SHA1 over it with the MIC field zero, first 16 bytes (AKM 2, key version 2).
void mic(const uint8_t kck[16], const uint8_t *eapol, size_t length, uint8_t out[20])
{
    static const uint8_t zero[kMicLength] = {};
    const uint8_t *parts[3] = {eapol, zero, eapol + kMicAt + kMicLength};
    const size_t lengths[3] = {kMicAt, kMicLength, length - kMicAt - kMicLength};
    crypto::hmacSha1(kck, 16, parts, lengths, 3, out);
}

bool hasSuite(const uint8_t *suites, size_t count, const uint8_t want[4])
{
    for (size_t i = 0; i < count; ++i) {
        if (!memcmp(suites + 4 * i, want, 4))
            return true;
    }
    return false;
}

}  // namespace

const uint8_t kRsnIe[22] = {48,   20,   1, 0, 0x00, 0x0F, 0xAC, 4,    1, 0, 0x00,
                            0x0F, 0xAC, 4, 1, 0,    0x00, 0x0F, 0xAC, 2, 0, 0};

struct Supplicant::Key {
    uint8_t src[6];
    uint8_t version;
    uint16_t info;
    const uint8_t *replay, *nonce, *rsc, *micField, *data, *eapol;
    size_t dataLength, eapolLength;
};

bool isEapol(const uint8_t *frame, size_t length)
{
    return length >= kEthHeader && be16(frame + 12) == kEthPae;
}

const uint8_t *element(const uint8_t *ies, size_t length, uint8_t id, size_t *elementLength)
{
    for (size_t k = 0; k + 2 <= length;) {
        const size_t end = k + 2 + ies[k + 1];
        if (end > length)
            return nullptr;
        if (ies[k] == id) {
            *elementLength = end - k;
            return ies + k;
        }
        k = end;
    }
    return nullptr;
}

const char *checkRsn(const uint8_t *rsn, size_t length)
{
    if (!rsn || length < 2 + 8)
        return "no RSN element";
    const uint8_t *b = rsn + 2;
    const size_t n = length - 2;
    if (le16(b) != 1)
        return "unknown RSN version";
    if (memcmp(b + 2, kSuiteCcmp, 4))
        return "group cipher is not CCMP (WPA/WPA2 mixed mode)";
    const size_t pairwise = le16(b + 6);
    if (8 + 4 * pairwise > n)
        return "truncated RSN element";
    if (!hasSuite(b + 8, pairwise, kSuiteCcmp))
        return "no CCMP pairwise cipher";
    const size_t akmAt = 8 + 4 * pairwise;
    if (akmAt + 2 > n)
        return "no key management in the RSN element";
    const size_t akm = le16(b + akmAt);
    if (akmAt + 2 + 4 * akm > n)
        return "truncated RSN element";
    if (!hasSuite(b + akmAt + 2, akm, kSuitePsk))
        return "no PSK key management (WPA3-only or enterprise)";
    const size_t capsAt = akmAt + 2 + 4 * akm;
    if (capsAt + 2 <= n && (le16(b + capsAt) & kRsnMfpRequired))
        return "management frame protection required";
    return nullptr;
}

void Supplicant::init(const uint8_t pmk[32], const uint8_t spa[6], Random random)
{
    wipe();
    memcpy(pmk_, pmk, 32);
    memcpy(spa_, spa, 6);
    random_ = random;
    setIe(kRsnIe, sizeof(kRsnIe));
}

void Supplicant::setIe(const uint8_t *ie, size_t length)
{
    ieLength_ = length < sizeof(ie_) ? length : sizeof(ie_);
    memcpy(ie_, ie, ieLength_);
}

void Supplicant::wipe()
{
    memset(pmk_, 0, sizeof(pmk_));
    memset(tptk_, 0, sizeof(tptk_));
    memset(ptk_, 0, sizeof(ptk_));
    memset(anonce_, 0, sizeof(anonce_));
    memset(snonce_, 0, sizeof(snonce_));
    memset(&gtk_, 0, sizeof(gtk_));
    haveAa_ = haveTptk_ = havePtk_ = haveReplay_ = haveGtk_ = false;
}

bool Supplicant::sameKey(const uint8_t *a, const uint8_t *b)
{
    return crypto::same(a, b, 48);
}

// PTK = PRF-384(PMK, "Pairwise key expansion", min(AA,SPA) || max(AA,SPA) || min(ANonce,SNonce) || max(...)):
// KCK 0..16, KEK 16..32, TK 32..48.
void Supplicant::derive(const uint8_t aa[6], uint8_t ptk[48]) const
{
    uint8_t data[76];
    const bool aaFirst = memcmp(aa, spa_, 6) < 0;
    const bool anonceFirst = memcmp(anonce_, snonce_, 32) < 0;
    memcpy(data, aaFirst ? aa : spa_, 6);
    memcpy(data + 6, aaFirst ? spa_ : aa, 6);
    memcpy(data + 12, anonceFirst ? anonce_ : snonce_, 32);
    memcpy(data + 44, anonceFirst ? snonce_ : anonce_, 32);
    crypto::prf(pmk_, 32, "Pairwise key expansion", data, sizeof(data), ptk, 48);
}

// Our EAPOL-Key frame to the authenticator, its MIC computed with `kck`.
void Supplicant::build(const Key &k, uint16_t info, const uint8_t nonce[32], const uint8_t *data, size_t dataLength,
                       const uint8_t kck[16], Step &step) const
{
    uint8_t *f = step.reply;
    const size_t eapolLength = kData + dataLength;
    memset(f, 0, kEthHeader + eapolLength);
    memcpy(f, k.src, 6);
    memcpy(f + 6, spa_, 6);
    f[12] = kEthPae >> 8;
    f[13] = kEthPae & 0xFF;
    uint8_t *e = f + kEthHeader;
    e[0] = k.version > 2 ? 2 : k.version;
    if (k.version < 1)
        e[0] = 1;
    e[1] = kEapolKey;
    e[2] = static_cast<uint8_t>((eapolLength - kHeader) >> 8);
    e[3] = static_cast<uint8_t>(eapolLength - kHeader);
    e[4] = kDescRsn;
    e[kInfo] = static_cast<uint8_t>(info >> 8);
    e[kInfo + 1] = static_cast<uint8_t>(info);
    // Key length 0 from a supplicant (802.11-2016 12.7.6.3); replay counter echoed.
    memcpy(e + kReplay, k.replay, 8);
    memcpy(e + kNonce, nonce, 32);
    e[kDataLength] = static_cast<uint8_t>(dataLength >> 8);
    e[kDataLength + 1] = static_cast<uint8_t>(dataLength);
    if (dataLength)
        memcpy(e + kData, data, dataLength);
    uint8_t m[20];
    mic(kck, e, eapolLength, m);
    memcpy(e + kMicAt, m, kMicLength);
    step.replyLength = kEthHeader + eapolLength;
}

// The GTK KDE (00-0F-AC type 1) in the key data, which the KEK wraps (RFC 3394).
const char *Supplicant::unwrapGtk(const Key &k, const uint8_t ptk[48], Gtk &gtk)
{
    if (!(k.info & kEncrypted))
        return "key data not encrypted";
    if (k.dataLength < 24 || k.dataLength > 256 || k.dataLength % 8)
        return "key data of an odd size";
    uint8_t plain[256];
    if (!crypto::keyUnwrap(ptk + 16, k.data, k.dataLength, plain))
        return "key data does not unwrap";
    const size_t n = k.dataLength - 8;
    for (size_t at = 0; at + 2 <= n;) {
        const uint8_t id = plain[at];
        const uint8_t length = plain[at + 1];
        if (id == 0xDD && length == 0)
            break;  // padding
        if (at + 2 + length > n)
            return "truncated key data element";
        const uint8_t *body = plain + at + 2;
        if (id == 0xDD && length >= 6 && body[0] == 0x00 && body[1] == 0x0F && body[2] == 0xAC && body[3] == 1) {
            if (length - 6 != 16)
                return "group key is not a 16-byte CCMP key";
            gtk.index = body[4] & 3;
            memcpy(gtk.key, body + 6, 16);
            memcpy(gtk.rsc, k.rsc, 6);
            memset(plain, 0, sizeof(plain));
            return nullptr;
        }
        at += 2 + length;
    }
    memset(plain, 0, sizeof(plain));
    return "no group key in the key data";
}

bool Supplicant::fresh(const Gtk &gtk)
{
    if (haveGtk_ && gtk.index == gtk_.index && crypto::same(gtk.key, gtk_.key, 16))
        return false;
    gtk_ = gtk;
    haveGtk_ = true;
    return true;
}

const char *Supplicant::receive(const uint8_t *frame, size_t length, Step &step)
{
    step.replyLength = 0;
    step.hasPtk = step.hasGtk = false;
    if (!isEapol(frame, length))
        return "not an EAPOL frame";
    const uint8_t *e = frame + kEthHeader;
    const size_t available = length - kEthHeader;
    if (available < kData || e[1] != kEapolKey || e[4] != kDescRsn)
        return "not an RSN EAPOL-Key frame";
    const size_t eapolLength = kHeader + be16(e + 2);
    if (eapolLength < kData || eapolLength > available)
        return "truncated EAPOL-Key frame";
    Key k;
    memcpy(k.src, frame + 6, 6);
    k.version = e[0];
    k.info = be16(e + kInfo);
    k.replay = e + kReplay;
    k.nonce = e + kNonce;
    k.rsc = e + kRsc;
    k.micField = e + kMicAt;
    k.dataLength = be16(e + kDataLength);
    k.data = e + kData;
    k.eapol = e;
    k.eapolLength = eapolLength;
    if (kData + k.dataLength > eapolLength)
        return "truncated key data";
    if ((k.info & kInfoVersion) != kVersionAes)
        return "key descriptor is not version 2 (HMAC-SHA1, AES)";
    if (haveAa_ && memcmp(aa_, k.src, 6))
        return "EAPOL-Key frame from another station";
    if (haveReplay_ && memcmp(k.replay, replay_, 8) <= 0)
        return "replay counter did not increase";

    const bool pairwise = (k.info & kPairwise) != 0;
    const bool withMic = (k.info & kMic) != 0;
    uint8_t m[20];
    if (pairwise && !withMic && (k.info & kAck)) {
        // Message 1: our nonce and the temporary PTK (a retransmitted message 1 keeps both).
        memcpy(aa_, k.src, 6);
        haveAa_ = true;
        if (!haveTptk_ || memcmp(k.nonce, anonce_, 32) || (havePtk_ && sameKey(tptk_, ptk_))) {
            memcpy(anonce_, k.nonce, 32);
            random_(snonce_, 32);
            derive(k.src, tptk_);
            haveTptk_ = true;
        }
        build(k, kVersionAes | kPairwise | kMic, snonce_, ie_, ieLength_, tptk_, step);
        return nullptr;
    }
    if (pairwise && withMic && (k.info & (kAck | kInstall)) == (kAck | kInstall)) {
        // Message 3: MIC under the temporary PTK, the GTK unwrapped; message 4 goes back in the clear before the keys
        // are installed.
        if (!haveTptk_)
            return "message 3 before message 1";
        if (memcmp(k.nonce, anonce_, 32))
            return "ANonce differs between messages 1 and 3";
        mic(tptk_, k.eapol, k.eapolLength, m);
        if (!crypto::same(m, k.micField, kMicLength))
            return "message 3 MIC mismatch";
        Gtk gtk;
        if (const char *why = unwrapGtk(k, tptk_, gtk))
            return why;
        memcpy(replay_, k.replay, 8);
        haveReplay_ = true;
        static const uint8_t zero[32] = {};
        build(k, kVersionAes | kPairwise | kMic | kSecure, zero, nullptr, 0, tptk_, step);
        if (!havePtk_ || !sameKey(ptk_, tptk_)) {
            memcpy(ptk_, tptk_, 48);
            havePtk_ = true;
            memcpy(step.tk, ptk_ + 32, 16);
            step.hasPtk = true;
        }
        if (fresh(gtk)) {
            step.gtk = gtk;
            step.hasGtk = true;
        }
        return nullptr;
    }
    if (!pairwise && withMic && (k.info & kAck)) {
        // Group key handshake message 1: a new GTK under the PTK.
        if (!havePtk_)
            return "group key before the pairwise handshake";
        mic(ptk_, k.eapol, k.eapolLength, m);
        if (!crypto::same(m, k.micField, kMicLength))
            return "group message 1 MIC mismatch";
        Gtk gtk;
        if (const char *why = unwrapGtk(k, ptk_, gtk))
            return why;
        memcpy(replay_, k.replay, 8);
        haveReplay_ = true;
        static const uint8_t zero[32] = {};
        build(k, kVersionAes | kMic | kSecure, zero, nullptr, 0, ptk_, step);
        if (fresh(gtk)) {
            step.gtk = gtk;
            step.hasGtk = true;
        }
        return nullptr;
    }
    return "unexpected EAPOL-Key message";
}

}  // namespace swlan::wpa
