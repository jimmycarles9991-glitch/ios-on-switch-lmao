/* SPDX-License-Identifier: MIT */
/* The shader compiler's jetsam band and memory limits, the same on every iOS version. iOS 17's BlueTool stand-in spawns
 * it with them (posix_spawnattr_setjetsam_ext, as xpcproxy does for launchd). iOS 27's launchd ignores a daemon plist's
 * JetsamProperties and takes them from the Supplemental cryptex's jetsam properties overlay
 * (overlay/supplemental/com.brunostjohn.switch.shadercompiler.jetsam.plist, merged into //overlay:jetsam_properties;
 * tools/package/shader_compiler_launch_test.py holds it to these). Without an entry there a daemon gets J181's Daemon
 * Override Global: band 40, ActiveSoft and InactiveHard 6 MB.
 *
 * The daemon declares no transactions (EnableTransactions false in its launchd plist, untracked on 17), so it is never
 * clean, ages into no idle band and is never idle-exited: the active limit is the one in force. Like Apple's compiler
 * services (J181's MTLCompilerService: ActiveSoft 150, InactiveHard 50) the active limit is soft, an EXC_RESOURCE
 * report and no kill: losing the compiler stalls every GPU client waiting on it. */
#ifndef SWITCH_SHADER_COMPILER_JETSAM_H
#define SWITCH_SHADER_COMPILER_JETSAM_H

/* J181's band for the always-on services the shell depends on (lsd, locationd, videocodecd, cameracaptured): above the
 * foreground app (100), below SpringBoard (160) and backboardd (170), whose GPU states it compiles */
#define SWITCH_SHADER_COMPILER_JETSAM_PRIORITY 140
/* MB. The daemon serves at 13.4 MB, and a compile adds its transient: directpack's peak footprint for one state alone
 * on the Mac, less its baseline, is a few MB for most of the corpus, 76-152 MB for RenderBox's uber_fragment and 327 MB
 * for QuartzCore's generic fixed_frag_lph_cpf (com.apple.coreanimation.PBGRAXm_A2Xghfc). The soft active limit, the one
 * in force, holds the daemon and one worst uber compile, of the order of J181's MTLCompilerService (150); a compile
 * past it costs one EXC_RESOURCE report. The hard inactive limit is a backstop that is never in force. */
#define SWITCH_SHADER_COMPILER_ACTIVE_LIMIT_MB   192
#define SWITCH_SHADER_COMPILER_INACTIVE_LIMIT_MB 512

#endif
