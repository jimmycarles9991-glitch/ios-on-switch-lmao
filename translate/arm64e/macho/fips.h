/*
 * corecrypto's FIPS integrity check (fipspost_post_integrity, through fipspost_get_hmac; the kext's and
 * libcorecrypto's are the same code): HMAC-SHA256, keyed with 32 zero bytes, over the first __text section (in segment
 * __TEXT or __TEXT_EXEC) of the first segment whose name starts with __TEXT, in load-command order. The MAC it
 * expects is __TEXT,__fips_hmacs. Translation rewrites that text, so a translated image carries the MAC of what it
 * runs, or the POST fails and the kernel panics ("FIPS Kernel POST Failed"). Only a MAC that held before is
 * rewritten: Apple's own caches keep stale ones in libcorecrypto's variants (_noasm, _trace), and so do we.
 */
#ifndef A64X_FIPS_H
#define A64X_FIPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A64X_FIPS_MAC_SIZE 32u

/* Where the check reads: the covered text and the expected MAC, as addresses and as offsets from the header. */
struct a64x_fips {
    uint64_t text_vmaddr, text_fileoff, text_size;
    uint64_t mac_vmaddr, mac_fileoff;
};

/* hdr: a mach_header_64 and its load commands (len bytes). 1 with *f filled, 0 for an image without the check. */
int a64x_fips_find(const uint8_t *hdr, size_t len, struct a64x_fips *f);

void a64x_fips_mac(const uint8_t *text, size_t size, uint8_t mac[A64X_FIPS_MAC_SIZE]);

/* mac holds the image's MAC: if it is that of original, it becomes that of translated (1); else it stays (0). */
int a64x_fips_rewrite(const uint8_t *original, const uint8_t *translated, size_t size, uint8_t mac[A64X_FIPS_MAC_SIZE]);

/* Whether now is the MAC a64x_fips_rewrite leaves, given the original image's MAC was. */
int a64x_fips_kept(const uint8_t *original, const uint8_t *translated, size_t size,
                   const uint8_t was[A64X_FIPS_MAC_SIZE], const uint8_t now[A64X_FIPS_MAC_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* A64X_FIPS_H */
