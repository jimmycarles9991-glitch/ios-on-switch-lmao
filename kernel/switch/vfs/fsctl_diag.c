// SPDX-License-Identifier: GPL-2.0-only
/*
 * A filesystem fsctl that fails (<switch/fsctl_diag.h>), debug kernels. Only commands the filesystem itself handles reach
 * here, and only APFS's ('J'): the line is for the purgeable mark (J 111) and its siblings. The caller's thread is in its
 * own fsctl, so reading the file's attributes and first bytes here costs it, and only when the call already failed.
 */
#include <sys/decmpfs.h>
#include <sys/ioccom.h>
#include <sys/proc.h>
#include <sys/uio_internal.h>
#include <sys/vnode_internal.h>
#include <sys/systm.h>
#include <sys/xattr.h>

#include <switch/fsctl_diag.h>

#include <kern/kalloc.h>
#include <os/log.h>

#if DEVELOPMENT || DEBUG

#define FSCTL_DIAG_PATH 96

static uint32_t fsctl_diag_marks, fsctl_diag_others;

static int fsctl_diag_xattr(vnode_t vp, const char *name, void *buf, size_t len, size_t *size, vfs_context_t ctx)
{
    UIO_STACKBUF(uio_buf, 1);
    uio_t uio = NULL;

    if (buf != NULL) {
        uio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_READ, uio_buf, sizeof(uio_buf));
        uio_addiov(uio, CAST_USER_ADDR_T(buf), len);
    }
    *size = len;
    return vn_getxattr(vp, name, uio, size, XATTR_NOSECURITY | XATTR_SHOWCOMPRESSION, ctx);
}

void switch_fsctl_failed(vnode_t vp, unsigned long cmd, const void *data, int error, vfs_context_t ctx)
{
    char process[32], content[96], compression[64], fork[48];
    char *path = NULL;
    const char *tail = "?";
    uint8_t head[0x48] = {0};
    uint64_t words[6] = {0};
    size_t xattr_size = 0;
    struct vnode_attr va;
    int len = MAXPATHLEN, resid = 0;
    void *xattr = NULL;
    enum switch_fsctl_line line;

    if (error == 0 || IOCGROUP(cmd) != 'J') {
        return;
    }
    line = switch_fsctl_line_for(
        cmd, os_atomic_inc_orig((cmd & 0xff) == SWITCH_FSCTL_MARK_PURGEABLE ? &fsctl_diag_marks : &fsctl_diag_others,
                                relaxed));
    if (line == SWITCH_FSCTL_NONE) {
        return;
    }
    if (line == SWITCH_FSCTL_LAST) {
        os_log(OS_LOG_DEFAULT, "SwitchFsctl: no more lines for refused APFS fsctls %s this boot",
               (cmd & 0xff) == SWITCH_FSCTL_MARK_PURGEABLE ? "'J' 111" : "other than 'J' 111");
        return;
    }
    if (data != NULL) {
        memcpy(words, data, MIN(sizeof(words), (size_t)IOCPARM_LEN(cmd)));
    }
    proc_selfname(process, sizeof(process));
    path = kalloc_data(MAXPATHLEN, Z_WAITOK);
    if (path != NULL && vn_getpath(vp, path, &len) == 0) {
        tail = strlen(path) > FSCTL_DIAG_PATH ? path + strlen(path) - FSCTL_DIAG_PATH : path;
    }
    if (line == SWITCH_FSCTL_BRIEF) {
        os_log(OS_LOG_DEFAULT, "SwitchFsctl: %s[%d] fsctl 0x%lx ('J' %lu) refused %d on ...%s", process, proc_selfpid(),
               cmd, cmd & 0xff, error, tail);
        if (path != NULL) {
            kfree_data(path, MAXPATHLEN);
        }
        return;
    }
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    VATTR_WANTED(&va, va_flags);
    if (vnode_getattr(vp, &va, ctx) != 0) {
        va.va_data_size = 0;
        va.va_flags = 0;
    }
    xattr = kalloc_data(MAX_DECMPFS_XATTR_SIZE, Z_WAITOK);
    if (xattr != NULL &&
        fsctl_diag_xattr(vp, DECMPFS_XATTR_NAME, xattr, MAX_DECMPFS_XATTR_SIZE, &xattr_size, ctx) == 0 &&
        xattr_size >= sizeof(decmpfs_disk_header)) {
        const decmpfs_disk_header *hdr = xattr;

        snprintf(compression, sizeof(compression), "decmpfs type %u, %llu bytes uncompressed", hdr->compression_type,
                 hdr->uncompressed_size);
    } else {
        strlcpy(compression, "no decmpfs header", sizeof(compression));
    }
    if (xattr != NULL) {
        kfree_data(xattr, MAX_DECMPFS_XATTR_SIZE);
    }
    if (fsctl_diag_xattr(vp, XATTR_RESOURCEFORK_NAME, NULL, 0, &xattr_size, ctx) == 0) {
        snprintf(fork, sizeof(fork), "resource fork %zu bytes", xattr_size);
    } else {
        strlcpy(fork, "no resource fork", sizeof(fork));
    }
    if (vnode_isreg(vp) && vn_rdwr(UIO_READ, vp, (caddr_t)head, (int)sizeof(head), 0, UIO_SYSSPACE, IO_NOAUTH,
                                   vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) == 0) {
        switch_fsctl_describe_content(head, sizeof(head) - (size_t)resid, content, sizeof(content));
    } else {
        strlcpy(content, vnode_isdir(vp) ? "a directory" : "unread", sizeof(content));
    }
    os_log(OS_LOG_DEFAULT,
           "SwitchFsctl: %s[%d] fsctl 0x%lx ('J' %lu) refused %d on ...%s: words 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx "
           "0x%llx; "
           "%lld bytes, "
           "st_flags 0x%x, %s, %s; content %s",
           process, proc_selfpid(), cmd, cmd & 0xff, error, tail, words[0], words[1], words[2], words[3], words[4],
           words[5], (long long)va.va_data_size, va.va_flags, compression, fork, content);
    if (path != NULL) {
        kfree_data(path, MAXPATHLEN);
    }
}

#endif /* DEVELOPMENT || DEBUG */
