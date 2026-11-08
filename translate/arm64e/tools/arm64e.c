#include "commands.h"

#include <stdio.h>
#include <string.h>

static const struct {
    const char *name;
    int (*run)(int, char **);
    const char *usage;
} commands[] = {
    {"translate", a64x_cmd_translate,
     "[-k] [-m MAP_PAGE (4096)] IN OUT [STUBS]   translate a Mach-O (thin or fat) as the loader does; write it and its "
     "stubs"},
    {"fixups", a64x_cmd_fixups, "IN [OUT]   count authenticated chained fixups; with OUT, write them as plain ones"},
    {"pretranslate", a64x_cmd_pretranslate,
     "[-r REPORT.json] IN OUT   a Mach-O translated in place, marked, re-signed"},
    {"dsc-apply", a64x_cmd_dsc_apply, "[-c CDHASH_PAIRS] PLAN CACHE_FILE OUT   one cache file, translated, re-signed"},
    {"dsc-verify", a64x_cmd_dsc_verify,
     "PLAN ORIGINAL_MAIN_CACHE TRANSLATED_DIR|-   a translated cache, from its bytes"},
    {"fips", a64x_cmd_fips,
     "HEADER...   per Mach-O header: the text corecrypto's FIPS check covers and its MAC's address"},
    {"fips-mac", a64x_cmd_fips_mac,
     "ORIGINAL TRANSLATED MAC   the MAC that check expects once ORIGINAL (text) is TRANSLATED, MAC its old one"},
    {"check", a64x_cmd_check, "[-a] FILE   what the loader does with a Mach-O or a fileset, by class"},
    {"xlate", a64x_cmd_xlate, "[-p PAGE] [-d WORDS] FILE|DIR...   translate standalone images and check every stub"},
    {"scan", a64x_cmd_scan, "[-u U] [-s S] [-S N] [-i] [-A] dsc CACHE | macho FILE...   ARMv8.1+ inventory"},
    {"kext", a64x_cmd_kext, "BLOB BLOB_VA JOBS STUBS_VA OUT_BLOB OUT_STUBS   kexts of a prelinked kernel"},
    {"plan", a64x_cmd_plan2, "[-o PLAN] [-d WORDS] CACHE   the dyld cache's translation plan for the kernel"},
    {"holes", a64x_cmd_holes, "CACHE   dead space in a dyld cache's text"},
    {"class", a64x_cmd_class, "< WORDS   the class of each hex word"},
    {"stub", a64x_cmd_stubdump, "WORD DEADMASK   one instruction's stub, as llvm-mc input"},
    {"gen-toptable", a64x_cmd_gen_toptable, "   regenerate a64x_classify.c's fast-reject table (all 2^32 words)"},
};

int main(int argc, char **argv)
{
    if (argc >= 2) {
        for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
            if (!strcmp(argv[1], commands[i].name))
                return commands[i].run(argc - 1, argv + 1);
        }
    }
    fprintf(stderr, "usage: arm64e COMMAND ARGS...\n");
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        fprintf(stderr, "  arm64e %s %s\n", commands[i].name, commands[i].usage);
    return 2;
}
