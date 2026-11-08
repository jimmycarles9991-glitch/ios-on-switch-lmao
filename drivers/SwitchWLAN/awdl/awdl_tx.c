// SPDX-License-Identifier: BSD-2-Clause-Patent
// Our PSFs and MIFs, field for field as OWL sends them (src/tx.c awdl_init_full_action_frame), which Apple devices
// synchronise to and elect with. The constant fields are OWL's values, which it took from Apple's frames.
#include "awdl_internal.h"

#define AWDL_ACTION_VERSION 0x10  // AWDL 1.0, the only action frame version (OWL AWDL_VERSION_COMPAT)
#define AWDL_PRESENCE_MODE  4u    // Apple uses no other value (Stute et al. §6.2)
#define AWDL_SYNC_FLAGS     0x1800u
#define AWDL_DPS_FLAGS \
    0x8f24u  // OWL's: country, social channel map, AWDL address, extension, and three bits
             // that add no field

static void write_service_params(struct awdl_core *c, struct awdl_writer *w)
{
    static const uint8_t unknown[3];
    awdl_w_begin(w, AWDL_TLV_SERVICE_PARAMS);
    awdl_w_bytes(w, unknown, sizeof unknown);
    awdl_w_u16(w, c->service_update_indicator);
    awdl_w_u32(w, 0);  // no value bitmask
    awdl_w_end(w);
}

static void write_ht_caps(struct awdl_core *c, struct awdl_writer *w)
{
    awdl_w_begin(w, AWDL_TLV_HT_CAPS);
    awdl_w_u16(w, 0);
    awdl_w_u16(w, 0x11ce);  // HT20/40, SM power save off, short GI 20, greenfield off
    awdl_w_u8(w, 0x1b);     // A-MPDU: 64 KiB, 8 us spacing
    const unsigned streams = c->cfg.ht_streams ? c->cfg.ht_streams : 1;
    for (unsigned i = 0; i < streams && i < 4; ++i)
        awdl_w_u8(w, 0xff);  // receive MCS 0-7 of each spatial stream
    awdl_w_u16(w, 0);
    awdl_w_end(w);
}

static void write_arpa(struct awdl_core *c, struct awdl_writer *w)
{
    awdl_w_begin(w, AWDL_TLV_ARPA);
    awdl_w_u8(w, 3);
    const size_t n = strnlen(c->cfg.hostname, sizeof c->cfg.hostname);
    awdl_w_u8(w, (uint8_t)n);
    awdl_w_bytes(w, c->cfg.hostname, n);
    awdl_w_u8(w, 0xc0);  // ".local", DNS-compressed
    awdl_w_u8(w, 0x0c);
    awdl_w_end(w);
}

static void write_data_path_state(struct awdl_core *c, struct awdl_writer *w)
{
    awdl_w_begin(w, AWDL_TLV_DATA_PATH_STATE);
    awdl_w_u16(w, AWDL_DPS_FLAGS);
    awdl_w_u8(w, (uint8_t)c->cfg.country[0]);
    awdl_w_u8(w, (uint8_t)c->cfg.country[1]);
    awdl_w_u8(w, 0);
    awdl_w_u16(w, awdl_social_map(c->cfg.seq));
    awdl_w_bytes(w, c->cfg.addr, 6);
    awdl_w_u16(w, 0);  // extension flags
    awdl_w_end(w);
}

void awdl_write_extras(struct awdl_core *c, struct awdl_writer *w, int mif)
{
    write_service_params(c, w);
    if (mif) {
        write_ht_caps(c, w);
        write_arpa(c, w);
    }
    write_data_path_state(c, w);
    awdl_w_begin(w, AWDL_TLV_VERSION);
    awdl_w_u8(w, c->cfg.awdl_version);
    awdl_w_u8(w, c->cfg.device_class);
    awdl_w_end(w);
}

size_t awdl_core_build_payload(struct awdl_core *c, uint8_t *buf, size_t size)
{
    struct awdl_writer w = {buf, size, 0, 0, 0};
    awdl_write_extras(c, &w, 1);
    return w.overflow ? 0 : w.at;
}

// TU until the next EAW starts, 1..64, and the TU of the first AW still left in this one (OWL awdl_sync_next_aw_tu
// and its remaining_aw_length).
static uint16_t down_counter(const struct awdl_core *c, uint64_t now)
{
    const uint64_t into = (now - awdl_eaw_start(c, now)) / AWDL_TU_US;
    return (uint16_t)(AWDL_EAW_TU - into % AWDL_EAW_TU);
}

static void write_sync_params(struct awdl_core *c, struct awdl_writer *w, uint64_t now)
{
    const uint16_t counter = down_counter(c, now);
    const uint16_t aw = awdl_core_aw_seq(c, now);
    const unsigned past_first = AWDL_EAW_TU - counter;
    awdl_w_begin(w, AWDL_TLV_SYNC_PARAMS);
    awdl_w_u8(w, awdl_core_channel_at(c, now).number);
    awdl_w_u16(w, counter);
    awdl_w_u8(w, awdl_master_chan(c));
    awdl_w_u8(w, 0);  // guard time
    awdl_w_u16(w, AWDL_AW_TU);
    awdl_w_u16(w, AWDL_PSF_INTERVAL_TU);
    awdl_w_u16(w, AWDL_SYNC_FLAGS);
    awdl_w_u16(w, AWDL_AW_TU);  // AW extension length
    awdl_w_u16(w, AWDL_AW_TU);  // AW common length
    awdl_w_u16(w, (uint16_t)(past_first < AWDL_AW_TU ? AWDL_AW_TU - past_first : 0));
    for (unsigned i = 0; i < 4; ++i)
        awdl_w_u8(w, AWDL_PRESENCE_MODE - 1);  // minimum and maximum extensions (multicast, unicast, action frames)
    awdl_w_bytes(w, c->adv.root, 6);
    awdl_w_u8(w, AWDL_PRESENCE_MODE);
    awdl_w_u8(w, 0);
    awdl_w_u16(w, aw);
    awdl_w_u16(w, aw);  // AP beacon alignment: Apple puts the AW number here too (OWL)
    awdl_w_chanseq(w, c->cfg.seq);
    awdl_w_u16(w, 0);
    awdl_w_end(w);
}

static void write_election(struct awdl_core *c, struct awdl_writer *w)
{
    awdl_w_begin(w, AWDL_TLV_ELECTION_PARAMS);
    awdl_w_u8(w, 0);
    awdl_w_u16(w, 0);
    awdl_w_u8(w, (uint8_t)c->adv.hops);
    awdl_w_u8(w, 0);
    awdl_w_bytes(w, c->adv.root, 6);
    awdl_w_u32(w, c->adv.root_metric);
    awdl_w_u32(w, c->adv.self_metric);
    awdl_w_u16(w, 0);
    awdl_w_end(w);

    awdl_w_begin(w, AWDL_TLV_CHANNEL_SEQUENCE);
    awdl_w_chanseq(w, c->cfg.seq);
    for (unsigned i = 0; i < 3; ++i)
        awdl_w_u8(w, 0);
    awdl_w_end(w);

    awdl_w_begin(w, AWDL_TLV_ELECTION_PARAMS_V2);
    awdl_w_bytes(w, c->adv.root, 6);
    awdl_w_bytes(w, c->adv.parent, 6);
    awdl_w_u32(w, c->adv.root_counter);
    awdl_w_u32(w, c->adv.hops);
    awdl_w_u32(w, c->adv.root_metric);
    awdl_w_u32(w, c->adv.self_metric);
    awdl_w_u32(w, 0);
    awdl_w_u32(w, 0);
    awdl_w_u32(w, c->self_counter);
    awdl_w_end(w);
}

size_t awdl_core_build_af(struct awdl_core *c, uint8_t subtype, uint64_t now, uint8_t *buf, size_t size)
{
    struct awdl_writer w = {buf, size, 0, 0, 0};
    uint8_t hdr[24] = {0xd0, 0x00};
    memset(hdr + 4, 0xff, 6);
    memcpy(hdr + 10, c->cfg.addr, 6);
    memcpy(hdr + 16, awdl_bssid, 6);
    awdl_put16(hdr + 22, (uint16_t)((c->mgmt_seq++ & 0xfffu) << 4));
    awdl_w_bytes(&w, hdr, 24);
    const uint8_t fixed[8] = {0x7f, awdl_oui[0], awdl_oui[1], awdl_oui[2], 0x08, AWDL_ACTION_VERSION, subtype, 0};
    awdl_w_bytes(&w, fixed, 8);
    // The down counter is a whole number of TU (OWL's rounding up); target_tx_time is the instant it is exact for and
    // phy_tx_time the build time, so a receiver that corrects by their difference places our EAW to the microsecond.
    const uint64_t target = awdl_eaw_start(c, now) + AWDL_EAW_US - (uint64_t)down_counter(c, now) * AWDL_TU_US;
    awdl_w_u32(&w, (uint32_t)now);
    awdl_w_u32(&w, (uint32_t)target);
    write_sync_params(c, &w, now);
    write_election(c, &w);
    awdl_write_extras(c, &w, subtype == AWDL_SUBTYPE_MIF);
    return w.overflow ? 0 : w.at;
}
