/* SHA-256 (FIPS 180-4), for code-directory page hashes and cdhashes on any host. */
#ifndef A64X_SHA256_H
#define A64X_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct a64x_sha256 {
#ifdef __APPLE__
    CC_SHA256_CTX cc;
#else
    uint32_t h[8];
    uint64_t length;
    uint8_t block[64];
    size_t used;
#endif
};

void a64x_sha256_init(struct a64x_sha256 *s);
void a64x_sha256_update(struct a64x_sha256 *s, const void *data, size_t size);
void a64x_sha256_final(struct a64x_sha256 *s, uint8_t digest[32]);
void a64x_sha256(const void *data, size_t size, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif

#endif /* A64X_SHA256_H */
