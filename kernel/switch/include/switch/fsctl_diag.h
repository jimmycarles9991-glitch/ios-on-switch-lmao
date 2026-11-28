// SPDX-License-Identifier: GPL-2.0-only
/*
 * A filesystem fsctl that fails (vfs/fsctl_diag.c, debug kernels): one "SwitchFsctl:" line naming the caller, the command
 * and its six argument words, and the file as the filesystem holds it (size, BSD flags, decmpfs compression and
 * resource fork) and as a reader sees its first bytes. SplashBoard drops every app snapshot whose purgeable mark fails,
 * and APFS refused each of boot 21's marks on a compressed file: the line says which file SplashBoard compressed (an
 * Apple texture without LZFSE inside, or another format) and what it asked APFS for. Every purgeable mark's outcome,
 * refused or not, is counted for kern.switch_fsctl_marks.
 *
 * The content description is plain C, so kernel/tests runs it on the host.
 */
#ifndef SWITCH_FSCTL_DIAG_H
#define SWITCH_FSCTL_DIAG_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

struct vnode;
struct vfs_context;

#if DEVELOPMENT || DEBUG
/* fsctl_internal, after the filesystem's VNOP_IOCTL: `data` holds the command's argument as the caller gave it. */
void switch_fsctl_failed(struct vnode *vp, unsigned long cmd, const void *data, int error, struct vfs_context *ctx);
#else
static inline void switch_fsctl_failed(__unused struct vnode *vp, __unused unsigned long cmd, __unused const void *data,
                                       __unused int error, __unused struct vfs_context *ctx)
{}
#endif

#define SWITCH_FSCTL_MARK_PURGEABLE 111u /* APFS's 'J' 111: mark a file or directory purgeable */
/* APFS's 'J' 68 (0xc0084a44, 8 bytes in and out): no header in the tree names it. On 24B119 SplashBoard sends it on its
 * snapshot files (refused 22), and OTACrashCopier on the DiagnosticReports/Retired directory (refused 1). What it does
 * on 21H461 is unseen. */
#define SWITCH_FSCTL_SET_PURGEABLE 68u
#define SWITCH_FSCTL_MARK_LINES    128u /* boot 21 refused 106 marks */
#define SWITCH_FSCTL_OTHER_LINES   32u

/* What became of a purgeable mark (kern.switch_fsctl_marks, debug kernels): APFS answers ENOTSUP for a volume without
 * the purgeable feature bits (a card imaged before the image builder set them) and refuses a decmpfs-compressed file. */
enum switch_fsctl_mark {
    SWITCH_FSCTL_MARK_OK,
    SWITCH_FSCTL_MARK_UNSUPPORTED,
    SWITCH_FSCTL_MARK_COMPRESSED,
    SWITCH_FSCTL_MARK_OTHER,
    SWITCH_FSCTL_MARK_KINDS,
};

/* A purgeable mark counted for kern.switch_fsctl_marks: every 'J' 111, and 'J' 68 on a regular file only, where it
 * marks a snapshot; on a directory it is another caller's (OTACrashCopier's) */
static inline int switch_fsctl_is_mark(unsigned long cmd, int regular)
{
    return (cmd & 0xff) == SWITCH_FSCTL_MARK_PURGEABLE || ((cmd & 0xff) == SWITCH_FSCTL_SET_PURGEABLE && regular);
}

static inline enum switch_fsctl_mark switch_fsctl_mark_for(int error, int compressed)
{
    if (error == 0) {
        return SWITCH_FSCTL_MARK_OK;
    }
    if (error == 45 /* ENOTSUP */) {
        return SWITCH_FSCTL_MARK_UNSUPPORTED;
    }
    return compressed ? SWITCH_FSCTL_MARK_COMPRESSED : SWITCH_FSCTL_MARK_OTHER;
}

enum switch_fsctl_line {
    SWITCH_FSCTL_FULL,  /* the file described */
    SWITCH_FSCTL_BRIEF, /* the caller, command and path only */
    SWITCH_FSCTL_LAST,  /* the budget is spent: say so once */
    SWITCH_FSCTL_NONE,
};

/* The line for the n-th refused APFS fsctl (from 0) of its budget: the purgeable mark has its own, so routine refusals of
 * other commands before the first app snapshot cannot use it up. */
static inline enum switch_fsctl_line switch_fsctl_line_for(unsigned long cmd, uint32_t n)
{
    const int mark = (cmd & 0xff) == SWITCH_FSCTL_MARK_PURGEABLE;
    const uint32_t cap = mark ? SWITCH_FSCTL_MARK_LINES : SWITCH_FSCTL_OTHER_LINES;

    if (n < cap) {
        return mark ? SWITCH_FSCTL_FULL : SWITCH_FSCTL_BRIEF;
    }
    return n == cap ? SWITCH_FSCTL_LAST : SWITCH_FSCTL_NONE;
}

static inline uint32_t switch_fsctl_le32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/*
 * What a file's first `n` bytes say: an Apple texture ("AAPL", ImageIO's com.apple.atx and SplashBoard's .ktx: GL
 * format, size and the word ImageIO sets for internal LZFSE), a KTX 1 texture (GL internal format and size), else the
 * first eight bytes. Returns the length written (snprintf's rules); the including file declares snprintf.
 */
static inline int switch_fsctl_describe_content(const uint8_t *b, size_t n, char *out, size_t size)
{
    static const uint8_t aapl[8] = {'A', 'A', 'P', 'L', '\r', '\n', 0x1a, '\n'};
    static const uint8_t ktx[8] = {0xab, 'K', 'T', 'X', ' ', '1', '1', 0xbb};

    if (n >= 0x48 && memcmp(b, aapl, sizeof(aapl)) == 0) {
        return snprintf(out, size, "AAPL texture format 0x%x %ux%u lzfse %u", switch_fsctl_le32(b + 0x20),
                        switch_fsctl_le32(b + 0x28), switch_fsctl_le32(b + 0x2c), switch_fsctl_le32(b + 0x44));
    }
    if (n >= 0x2c && memcmp(b, ktx, sizeof(ktx)) == 0) {
        return snprintf(out, size, "KTX 1 texture internal format 0x%x %ux%u", switch_fsctl_le32(b + 0x1c),
                        switch_fsctl_le32(b + 0x24), switch_fsctl_le32(b + 0x28));
    }
    if (n >= 8) {
        return snprintf(out, size, "bytes %02x%02x%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6],
                        b[7]);
    }
    return snprintf(out, size, "%zu bytes", n);
}

__END_DECLS

#endif /* SWITCH_FSCTL_DIAG_H */
