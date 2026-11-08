#include "sd_reference.h"

uint32_t switch_sd_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t crc = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static uint32_t le32(const volatile uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const volatile uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint64_t le64(const volatile uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int validate_reference(const volatile uint8_t *page, uint32_t bytes, uint32_t expected_commit,
                              struct switch_sd_reference *output, int card)
{
    if (!page || !output || bytes != SWITCH_SD_REFERENCE_BYTES || expected_commit > 1)
        return SWITCH_SD_REF_ARGUMENT;
    const uint32_t expected[10] = {
        SWITCH_SD_REFERENCE_MAGIC, card ? 2u : 1u, card ? 0x500u : 0x400u, 0x200, 0x200, 0x700b0000, 0, 1, 1, 0};
    for (unsigned i = 0; i < 10; ++i) {
        if (le32(page + 4 * (uintptr_t)i) != expected[i])
            return SWITCH_SD_REF_HEADER;
    }
    if (le32(page + SWITCH_SD_REFERENCE_COMMIT_OFFSET) != expected_commit)
        return SWITCH_SD_REF_HEADER;
    struct switch_sd_reference result;
    result.crc32 = be32(page + 0x28);
    for (unsigned i = 0x30; i < 0x200; ++i) {
        if (page[i])
            return SWITCH_SD_REF_PADDING;
    }
    for (unsigned i = card ? 0x500 : 0x400; i < SWITCH_SD_REFERENCE_BYTES; ++i) {
        if (page[i])
            return SWITCH_SD_REF_PADDING;
    }
    for (unsigned i = 0; i < 512; ++i)
        result.sector[i] = page[SWITCH_SD_REFERENCE_DATA_OFFSET + i];
    if (switch_sd_crc32(result.sector, 512) != result.crc32)
        return SWITCH_SD_REF_CRC;
    for (unsigned i = 0; i < 10; ++i) {
        if (le32(page + 4 * (uintptr_t)i) != expected[i])
            return SWITCH_SD_REF_CHANGED;
    }
    if (be32(page + 0x28) != result.crc32 || le32(page + SWITCH_SD_REFERENCE_COMMIT_OFFSET) != expected_commit)
        return SWITCH_SD_REF_CHANGED;
    output->crc32 = result.crc32;
    for (unsigned i = 0; i < 512; ++i)
        output->sector[i] = result.sector[i];
    return SWITCH_SD_REF_OK;
}

int switch_sd_reference_validate(const volatile uint8_t *page, uint32_t bytes, uint32_t expected_commit,
                                 struct switch_sd_reference *output)
{
    return validate_reference(page, bytes, expected_commit, output, 0);
}

int switch_sd_card_reference_validate(const volatile uint8_t *page, uint32_t bytes, uint32_t expected_commit,
                                      struct switch_sd_reference *output, struct switch_sd_card *card)
{
    if (!page || !output || !card || bytes != SWITCH_SD_REFERENCE_BYTES || expected_commit > 1)
        return SWITCH_SD_REF_ARGUMENT;
    uint8_t raw[SWITCH_SD_CARD_BYTES];
    for (unsigned i = 0; i < sizeof raw; ++i)
        raw[i] = page[SWITCH_SD_CARD_OFFSET + i];
    const uint32_t fixed[] = {SWITCH_SD_CARD_MAGIC, 1, 0x100, 1, 0x700b0000, 1, 512, 1};
    for (unsigned i = 0; i < 8; ++i) {
        if (le32(raw + 4 * (uintptr_t)i) != fixed[i])
            return SWITCH_SD_REF_CARD;
    }
    if (le32(raw + 0xf0) != switch_sd_crc32(raw, 0xf0))
        return SWITCH_SD_REF_CRC;
    for (unsigned i = 0xa0; i < 0xf0; ++i) {
        if (raw[i])
            return SWITCH_SD_REF_PADDING;
    }
    for (unsigned i = 0xf4; i < 0x100; ++i) {
        if (raw[i])
            return SWITCH_SD_REF_PADDING;
    }
    if (le32(raw + 0x3c))
        return SWITCH_SD_REF_PADDING;
    /* Identity of the existing OLED BL33 whose exporter ABI was reviewed. */
    const uint8_t firmware_hash[32] = {0xb4, 0x92, 0xa4, 0x01, 0x75, 0x80, 0xca, 0x11, 0xc9, 0xcd, 0x49,
                                       0x07, 0x5a, 0x89, 0x9a, 0x65, 0xf6, 0x20, 0x69, 0xd3, 0x75, 0x06,
                                       0x15, 0x21, 0x7f, 0xc1, 0x03, 0x08, 0x95, 0x4c, 0x9f, 0xaa};
    for (unsigned i = 0; i < 32; ++i) {
        if (raw[0x80 + i] != firmware_hash[i])
            return SWITCH_SD_REF_CARD;
    }
    /* The object pointers must remain in the excluded firmware memory. They
     * are never used as addresses here or by the native block reader. */
    for (unsigned i = 0x60; i <= 0x70; i += 8) {
        uint64_t p = le64(raw + i);
        if (p < UINT64_C(0xf0000000) || p >= UINT64_C(0xffd00000) || (p & 7))
            return SWITCH_SD_REF_CARD;
    }
    if (le64(raw + 0x60) == le64(raw + 0x68))
        return SWITCH_SD_REF_CARD;
    struct switch_sd_card value = {0};
    value.capacity_sectors = le64(raw + 0x20);
    value.capacity_bytes = le64(raw + 0x28);
    value.ocr = le32(raw + 0x30);
    value.rca = le32(raw + 0x34);
    value.version = le32(raw + 0x38);
    for (unsigned i = 0; i < 4; ++i) {
        value.csd[i] = le32(raw + 0x40 + 4 * (uintptr_t)i);
        value.cid[i] = le32(raw + 0x50 + 4 * (uintptr_t)i);
    }
    /* Pinned mmc_startup stores the 128-bit CSD in most-significant-word
     * order. CSDv2 C_SIZE[69:48] gives (C_SIZE+1)*1024 512-byte sectors. */
    uint64_t csize = ((uint64_t)(value.csd[1] & 0x3f) << 16) | (value.csd[2] >> 16);
    uint64_t sectors = (csize + 1) * 1024;
    if ((value.ocr & 0xc0000000u) != 0xc0000000u || !value.rca || value.rca > 0xffff ||
        !(value.version & 0x80000000u) || (value.csd[0] >> 30) != 1 || ((value.csd[1] >> 16) & 0xf) != 9 || !sectors ||
        sectors > UINT64_C(0x100000000) || value.capacity_sectors != sectors || value.capacity_bytes != sectors * 512 ||
        !(value.cid[0] | value.cid[1] | value.cid[2] | value.cid[3]))
        return SWITCH_SD_REF_CARD;
    struct switch_sd_reference reference;
    int error = validate_reference(page, bytes, expected_commit, &reference, 1);
    if (error)
        return error;
    for (unsigned i = 0; i < sizeof raw; ++i) {
        if (raw[i] != page[SWITCH_SD_CARD_OFFSET + i])
            return SWITCH_SD_REF_CHANGED;
    }
    output->crc32 = reference.crc32;
    for (unsigned i = 0; i < 512; ++i)
        output->sector[i] = reference.sector[i];
    card->capacity_sectors = value.capacity_sectors;
    card->capacity_bytes = value.capacity_bytes;
    card->ocr = value.ocr;
    card->rca = value.rca;
    card->version = value.version;
    for (unsigned i = 0; i < 4; ++i) {
        card->csd[i] = value.csd[i];
        card->cid[i] = value.cid[i];
    }
    return SWITCH_SD_REF_OK;
}
