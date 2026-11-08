#include "fips.h"

#include "sha256.h"

#include <string.h>

#define LC_SEGMENT_64 0x19u

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int named(const uint8_t *field, const char *name)
{
    return !strncmp((const char *)field, name, 16);
}

/* The section_64 for which match() holds, in segments whose name starts with __TEXT; NULL if none. */
static const uint8_t *text_section(const uint8_t *hdr, size_t len, int (*match)(const uint8_t *section))
{
    const uint32_t ncmds = rd32(hdr + 16);
    size_t at = 32;
    for (uint32_t i = 0; i < ncmds && len - at >= 8; i++) {
        const uint32_t cmd = rd32(hdr + at), cmdsize = rd32(hdr + at + 4);
        if (cmdsize < 8 || cmdsize > len - at)
            return NULL;
        if (cmd == LC_SEGMENT_64 && cmdsize >= 72 && !strncmp((const char *)hdr + at + 8, "__TEXT", 6)) {
            const uint32_t nsects = rd32(hdr + at + 64);
            for (uint32_t k = 0; k < nsects && k < (cmdsize - 72) / 80; k++) {
                if (match(hdr + at + 72 + 80 * k))
                    return hdr + at + 72 + 80 * k;
            }
        }
        at += cmdsize;
    }
    return NULL;
}

static int is_text(const uint8_t *s)
{
    return named(s, "__text") && (named(s + 16, "__TEXT") || named(s + 16, "__TEXT_EXEC"));
}

static int is_mac(const uint8_t *s)
{
    return named(s, "__fips_hmacs") && named(s + 16, "__TEXT") && rd64(s + 40) == A64X_FIPS_MAC_SIZE;
}

int a64x_fips_find(const uint8_t *hdr, size_t len, struct a64x_fips *f)
{
    if (len < 32 || rd32(hdr) != 0xFEEDFACFu || rd32(hdr + 20) > len - 32)
        return 0;
    len = 32 + rd32(hdr + 20);
    const uint8_t *text = text_section(hdr, len, is_text), *mac = text_section(hdr, len, is_mac);
    if (!text || !mac)
        return 0;
    f->text_vmaddr = rd64(text + 32);
    f->text_size = rd64(text + 40);
    f->text_fileoff = rd32(text + 48);
    f->mac_vmaddr = rd64(mac + 32);
    f->mac_fileoff = rd32(mac + 48);
    return 1;
}

void a64x_fips_mac(const uint8_t *text, size_t size, uint8_t mac[A64X_FIPS_MAC_SIZE])
{
    uint8_t pad[64], inner[32];
    struct a64x_sha256 s;
    /* HMAC with a key of 32 zero bytes: key ^ ipad and key ^ opad are the pads themselves. */
    memset(pad, 0x36, sizeof(pad));
    a64x_sha256_init(&s);
    a64x_sha256_update(&s, pad, sizeof(pad));
    a64x_sha256_update(&s, text, size);
    a64x_sha256_final(&s, inner);
    memset(pad, 0x5C, sizeof(pad));
    a64x_sha256_init(&s);
    a64x_sha256_update(&s, pad, sizeof(pad));
    a64x_sha256_update(&s, inner, sizeof(inner));
    a64x_sha256_final(&s, mac);
}

int a64x_fips_rewrite(const uint8_t *original, const uint8_t *translated, size_t size, uint8_t mac[A64X_FIPS_MAC_SIZE])
{
    uint8_t held[A64X_FIPS_MAC_SIZE];
    a64x_fips_mac(original, size, held);
    if (memcmp(held, mac, sizeof(held)))
        return 0;
    a64x_fips_mac(translated, size, mac);
    return 1;
}

int a64x_fips_kept(const uint8_t *original, const uint8_t *translated, size_t size,
                   const uint8_t was[A64X_FIPS_MAC_SIZE], const uint8_t now[A64X_FIPS_MAC_SIZE])
{
    uint8_t expect[A64X_FIPS_MAC_SIZE];
    memcpy(expect, was, sizeof(expect));
    (void)a64x_fips_rewrite(original, translated, size, expect);
    return !memcmp(expect, now, sizeof(expect));
}
