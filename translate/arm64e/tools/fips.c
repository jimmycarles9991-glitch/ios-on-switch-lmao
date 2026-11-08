#include "commands.h"

#include "../macho/fips.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *buf = NULL;
    if (!fseek(f, 0, SEEK_END)) {
        const long n = ftell(f);
        if (n >= 0 && !fseek(f, 0, SEEK_SET) && (buf = malloc((size_t)n + 1)) &&
            fread(buf, 1, (size_t)n, f) == (size_t)n) {
            *size = (size_t)n;
        } else {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    return buf;
}

/* arm64e fips HEADER...: per file, "TEXT_VMADDR TEXT_SIZE MAC_VMADDR" of corecrypto's integrity check, or "-". */
int a64x_cmd_fips(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: arm64e fips HEADER...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        size_t size = 0;
        uint8_t *hdr = slurp(argv[i], &size);
        struct a64x_fips f;
        if (!hdr) {
            perror(argv[i]);
            return 1;
        }
        if (a64x_fips_find(hdr, size, &f))
            printf("%#" PRIx64 " %#" PRIx64 " %#" PRIx64 "\n", f.text_vmaddr, f.text_size, f.mac_vmaddr);
        else
            printf("-\n");
        free(hdr);
    }
    return 0;
}

/* arm64e fips-mac ORIGINAL TRANSLATED MAC: the MAC an image with that check keeps once ORIGINAL (its text) is
 * TRANSLATED, MAC (hex) being the one it had (macho/fips.h a64x_fips_rewrite), as hex. */
int a64x_cmd_fips_mac(int argc, char **argv)
{
    size_t size = 0, size2 = 0;
    uint8_t *original = argc == 4 ? slurp(argv[1], &size) : NULL,
            *translated = original ? slurp(argv[2], &size2) : NULL;
    uint8_t mac[A64X_FIPS_MAC_SIZE];
    int ok = translated && size == size2 && strlen(argv[3]) == 2 * sizeof(mac);
    for (unsigned i = 0; ok && i < sizeof(mac); i++)
        ok = sscanf(argv[3] + 2 * i, "%2hhx", &mac[i]) == 1;
    if (!ok) {
        fprintf(stderr, "usage: arm64e fips-mac ORIGINAL TRANSLATED MAC (texts of one size, MAC as hex)\n");
        free(original);
        free(translated);
        return 2;
    }
    (void)a64x_fips_rewrite(original, translated, size, mac);
    for (unsigned i = 0; i < sizeof(mac); i++)
        printf("%02x", mac[i]);
    printf("\n");
    free(original);
    free(translated);
    return 0;
}
