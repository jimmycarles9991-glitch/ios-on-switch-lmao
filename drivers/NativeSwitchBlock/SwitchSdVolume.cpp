// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchSdVolumeInternal.h"
#include <string.h>

namespace {
uint16_t le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t le32(const uint8_t *p)
{
    return le16(p) | (static_cast<uint32_t>(le16(p + 2)) << 16);
}

uint64_t le64(const uint8_t *p)
{
    return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32);
}

bool powerOfTwo(uint32_t n)
{
    return n && !(n & (n - 1));
}

bool fat32(const uint8_t *b, uint64_t sectors)
{
    const uint32_t bytes = le16(b + 11), cluster = b[13], reserved = le16(b + 14), fats = b[16];
    const uint32_t total = le32(b + 32), fat = le32(b + 36), root = le32(b + 44);
    if (bytes < 512 || bytes > 4096 || !powerOfTwo(bytes) || !powerOfTwo(cluster) || cluster > 128 || !reserved ||
        !fats || fats > 2 || !fat || le16(b + 17) || le16(b + 19) || le16(b + 22) || le16(b + 42) ||
        total > sectors / (bytes / 512))
        return false;
    const uint64_t first = reserved + static_cast<uint64_t>(fats) * fat;
    if (first >= total)
        return false;
    const uint64_t clusters = (total - first) / cluster;
    return clusters >= 65525 && clusters <= 0x0fffffee && root >= 2 && root < clusters + 2 &&
           static_cast<uint64_t>(fat) * bytes / 4 >= clusters + 2;
}

bool exfat(const uint8_t *b, uint64_t sectors)
{
    static const uint8_t reserved[53] = {};
    const uint32_t shift = b[108], clusterShift = b[109], fats = b[110];
    if (memcmp(b + 11, reserved, sizeof(reserved)) || le16(b + 104) != 0x100 || shift < 9 || shift > 12 ||
        clusterShift > 25 - shift || !fats || fats > 2)
        return false;
    const uint64_t length = le64(b + 72);
    const uint32_t offset = le32(b + 80), fat = le32(b + 84), heap = le32(b + 88);
    const uint64_t clusters = le32(b + 92), root = le32(b + 96);
    if (length < (UINT64_C(1) << (20 - shift)) || length > (sectors >> (shift - 9)) || offset < 24 || !fat ||
        offset + static_cast<uint64_t>(fat) * fats > heap || heap >= length)
        return false;
    constexpr uint64_t maxClusters = 0xfffffff5;
    const uint64_t available = (length - heap) >> clusterShift;
    const uint64_t expected = available < maxClusters ? available : maxClusters;
    return clusters && clusters == expected && clusters + 2 <= (static_cast<uint64_t>(fat) << (shift - 2)) &&
           root >= 2 && root < clusters + 2;
}

enum class MbrMatch : uint8_t {
    Invalid,
    None,
    Mirror,
};

struct MbrResult {
    MbrMatch match;
    uint8_t type;
};

bool validMbr(const uint8_t *mbr, uint64_t capacity)
{
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *p = mbr + 446 + 16 * i;
        const uint8_t type = p[4];
        const uint64_t first = le32(p + 8), count = le32(p + 12);
        if (p[0] != 0 && p[0] != 0x80)
            return false;
        if (!type) {
            if (first || count || p[0])
                return false;
            continue;
        }
        if (type == 0xee) {
            // Some partitioners saturate protective 0xee SizeInLBA to UINT32_MAX even on cards under 2 TiB.
            if (first != 1 || !count || (count != UINT32_MAX && count > capacity - first))
                return false;
            continue;
        }
        if (!first || !count || first >= capacity || count > capacity - first)
            return false;
    }
    return true;
}

MbrResult matchMbr(const uint8_t *mbr, const switch_gpt_extent &data)
{
    uint8_t mirror = 0;
    const uint64_t length = data.last - data.first + 1;
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *p = mbr + 446 + 16 * i;
        const uint8_t type = p[4];
        if (!type || type == 0xee)
            continue;
        const uint64_t first = le32(p + 8), count = le32(p + 12);
        if (first > data.last || first + count <= data.first)
            continue;
        if (mirror || first != data.first || count != length || (type != 0x0b && type != 0x0c && type != 0x07))
            return {MbrMatch::Invalid, 0};
        mirror = type;
    }
    return mirror ? MbrResult{MbrMatch::Mirror, mirror} : MbrResult{MbrMatch::None, 0};
}

void copyCid(const switch_sd_card &card, uint8_t cid[16])
{
    for (unsigned i = 0; i < 15; ++i)
        cid[i] = static_cast<uint8_t>(card.cid[i / 4] >> (24 - (i % 4) * 8));
    cid[15] = 0;
}
}  // namespace

bool switch_sd_handoff_cid(const volatile uint8_t *handoff, uint8_t cid[16])
{
    switch_sd_reference reference;
    switch_sd_card card;
    if (!cid || switch_sd_card_reference_validate(handoff, SWITCH_SD_REFERENCE_BYTES, 1, &reference, &card))
        return false;
    copyCid(card, cid);
    return true;
}

int switch_sd_files_select(switch_sd_read_sector_fn read, void *context, const volatile uint8_t *handoff,
                           uint64_t root_first, uint64_t root_sectors, switch_gpt_workspace *work,
                           switch_sd_files_slice *result)
{
    if (!read || !work || !result || !root_sectors)
        return SWITCH_GPT_ARGUMENT;
    switch_sd_reference reference;
    switch_sd_card card;
    if (switch_sd_card_reference_validate(handoff, SWITCH_SD_REFERENCE_BYTES, 1, &reference, &card) ||
        (card.csd[3] & 0x3000))
        return SWITCH_GPT_PROBE;
    if (read(context, 0, work->sectors[0]))
        return SWITCH_GPT_IO;
    if (memcmp(work->sectors[0], reference.sector, sizeof(reference.sector)))
        return SWITCH_GPT_MISMATCH;
    uint32_t used = 0;
    int error = switch_gpt_scan(read, context, card.capacity_sectors, work, &used);
    if (error)
        return error;
    uint32_t rootIndex = used;
    for (uint32_t i = 0; i < used; ++i) {
        const auto &entry = work->used[i];
        if (root_first == entry.first && root_sectors == entry.last - entry.first + 1)
            rootIndex = i;
    }
    if (rootIndex == used)
        return SWITCH_GPT_MISMATCH;
    if (!validMbr(reference.sector, card.capacity_sectors))
        return SWITCH_GPT_MBR;

    struct Candidate {
        uint32_t index;
        bool exfat;
    };

    Candidate chosen{}, hos{}, mirrored{};
    uint32_t count = 0, hosCount = 0, mirroredCount = 0;
    bool sawMbrReject = false, sawProbeReject = false;

    for (uint32_t i = 0; i < used; ++i) {
        if (i == rootIndex)
            continue;
        const auto &entry = work->used[i];
        if (!entry.basic_data || !entry.writable)
            continue;
        const MbrResult mbr = matchMbr(reference.sector, entry);
        if (mbr.match == MbrMatch::Invalid) {
            sawMbrReject = true;
            continue;
        }
        uint8_t *boot = work->sectors[0];
        if (read(context, entry.first, boot))
            return SWITCH_GPT_IO;
        if (boot[510] != 0x55 || boot[511] != 0xaa) {
            sawProbeReject = true;
            continue;
        }
        const uint64_t sectors = entry.last - entry.first + 1;
        const bool isExfat = !memcmp(boot + 3, "EXFAT   ", 8);
        if (isExfat) {
            if ((mbr.match == MbrMatch::Mirror && mbr.type == 0x0b) || !exfat(boot, sectors)) {
                sawProbeReject = true;
                continue;
            }
        } else if ((mbr.match == MbrMatch::Mirror && mbr.type == 0x07) || !fat32(boot, sectors)) {
            sawProbeReject = true;
            continue;
        }
        const Candidate candidate = {i, isExfat};
        chosen = candidate;
        ++count;
        if (entry.hos_data) {
            hos = candidate;
            ++hosCount;
        }
        if (mbr.match == MbrMatch::Mirror) {
            mirrored = candidate;
            ++mirroredCount;
        }
    }

    if (!count)
        return sawMbrReject ? SWITCH_GPT_MBR : sawProbeReject ? SWITCH_GPT_PROBE : SWITCH_GPT_NOT_FOUND;
    if (count > 1) {
        if (hosCount == 1)
            chosen = hos;
        else if (!hosCount && mirroredCount == 1)
            chosen = mirrored;
        else
            return SWITCH_GPT_AMBIGUOUS;
    }

    const auto &data = work->used[chosen.index];
    result->exfat = chosen.exfat;
    result->first_sector = data.first;
    result->sector_count = data.last - data.first + 1;
    memcpy(result->unique_guid, data.guid, sizeof(data.guid));
    copyCid(card, result->cid);
    return SWITCH_GPT_OK;
}
