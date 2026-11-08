/*
 * The subcommands of `arm64e` (tools/arm64e.c); argv[0] is the subcommand's name. Besides translate and fixups (over
 * macho/), each is a measurement tool over the core whose output README.md quotes.
 */
#ifndef ARM64E_TOOLS_COMMANDS_H
#define ARM64E_TOOLS_COMMANDS_H

int a64x_cmd_check(int argc, char **argv);        /* a64check.c: what the loader does with a Mach-O or a fileset */
int a64x_cmd_dsc_apply(int argc, char **argv);    /* dsc.c: one dyld cache file translated with its plan */
int a64x_cmd_dsc_verify(int argc, char **argv);   /* dsc.c: a translated dyld cache against the original */
int a64x_cmd_class(int argc, char **argv);        /* a64class.c: class of each hex word on stdin */
int a64x_cmd_fips(int argc, char **argv);         /* fips.c: where corecrypto's FIPS integrity check reads */
int a64x_cmd_fips_mac(int argc, char **argv);     /* fips.c: the MAC that check computes */
int a64x_cmd_gen_toptable(int argc, char **argv); /* gen_toptable.c: the classifier's fast-reject table */
int a64x_cmd_holes(int argc, char **argv);        /* a64holes.c: dead space in a dyld cache's text */
int a64x_cmd_kext(int argc, char **argv);         /* a64kext.c: kexts of a prelinked kernel, kernel mode */
int a64x_cmd_plan2(int argc, char **argv);        /* a64plan2.c: the dyld cache's translation plan */
int a64x_cmd_scan(int argc, char **argv);         /* a64scan.c: inventory of ARMv8.1+ instructions */
int a64x_cmd_stubdump(int argc, char **argv);     /* a64stubdump.c: one instruction's stub as llvm-mc input */
int a64x_cmd_xlate(int argc, char **argv);        /* a64xlate.c: standalone images, as the kernel loads them */
int a64x_cmd_translate(int argc, char **argv);    /* translate.c: write a translated image and its stubs */
int a64x_cmd_fixups(int argc, char **argv);       /* translate.c: authenticated chained fixups as plain ones */
int a64x_cmd_pretranslate(int argc, char **argv); /* pretranslate.c: a Mach-O translated in place, re-signed */

#endif /* ARM64E_TOOLS_COMMANDS_H */
