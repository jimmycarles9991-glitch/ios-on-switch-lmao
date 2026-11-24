// SPDX-License-Identifier: BSD-2-Clause-Patent
#if DEVELOPMENT || DEBUG
#include <IOKit/IOLib.h>
#include <libkern/crypto/sha2.h>
#include <os/log.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include "SwitchWLANRomState.h"

namespace {
struct SwitchWLANRom {
    size_t length;
    uint32_t overwritten;
};

switch_wlan_rom_state switch_rom_state;

int switch_rom_write(const char *path, const void *bytes, size_t length)
{
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    int error = vnode_open(path, O_CREAT | FWRITE | O_NOFOLLOW, 0644, 0, &vp, ctx);
    if (error)
        return error;
    int resid = 0;
    error =
        vn_rdwr(UIO_WRITE, vp, reinterpret_cast<caddr_t>(reinterpret_cast<uintptr_t>(bytes)), static_cast<int>(length),
                0, UIO_SYSSPACE, IO_NOAUTH | IO_UNIT, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
    if (!error && resid)
        error = EIO;
    if (!error) {
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_SET(&va, va_data_size, static_cast<uint64_t>(length));
        error = vnode_setattr(vp, &va, ctx);
    }
    if (!error)
        error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
    if (!error)
        error = VNOP_IOCTL(vp, F_FULLFSYNC, nullptr, 0, ctx);
    const int closeError = vnode_close(vp, FWRITE, ctx);
    return error ? error : closeError;
}

void switch_rom_log(const uint8_t *bytes, size_t length, uint32_t overwritten, const char *hash)
{
    os_log(OS_LOG_DEFAULT, "SwitchWLAN: ROM capture begin: %u bytes, overwritten prefix %u, sha256 %{public}s",
           static_cast<unsigned>(length), overwritten, hash);
    for (size_t offset = 0; offset < length; offset += 64) {
        const size_t count = length - offset < 64 ? length - offset : 64;
        char hex[129];
        for (size_t i = 0; i < count; ++i)
            snprintf(hex + 2 * i, 3, "%02x", bytes[offset + i]);
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: ROM chunk: %08x %{public}s", static_cast<unsigned>(offset), hex);
    }
    os_log(OS_LOG_DEFAULT, "SwitchWLAN: ROM capture end: %{public}s", hash);
}

void switch_rom_hash(const uint8_t *bytes, size_t length, char hash[65])
{
    uint8_t digest[32];
    SHA256_CTX context;
    SHA256_Init(&context);
    SHA256_Update(&context, bytes, length);
    SHA256_Final(digest, &context);
    for (size_t i = 0; i < sizeof(digest); ++i)
        snprintf(hash + 2 * i, 3, "%02x", digest[i]);
}

void switch_rom_persist()
{
    auto *pending = static_cast<SwitchWLANRom *>(switch_wlan_rom_take(&switch_rom_state));
    if (!pending)
        return;
    const auto *bytes = reinterpret_cast<const uint8_t *>(pending + 1);
    char hash[65], metadata[192];
    switch_rom_hash(bytes, pending->length, hash);
    const int length =
        snprintf(metadata, sizeof(metadata), "{\"bytes\":%zu,\"overwritten_prefix_bytes\":%u,\"sha256\":\"%s\"}\n",
                 pending->length, pending->overwritten, hash);
    int error = switch_rom_write("/private/var/Switch/wlan-rom.bin", bytes, pending->length);
    if (!error)
        error = switch_rom_write("/private/var/Switch/wlan-rom.json", metadata, static_cast<size_t>(length));
    if (error) {
        IOLog("SwitchWLAN: ROM file write failed (%d); capture follows in the log\n", error);
        switch_rom_log(bytes, pending->length, pending->overwritten, hash);
    } else {
        os_log(OS_LOG_DEFAULT,
               "SwitchWLAN: ROM saved: wlan-rom.bin, %u bytes, overwritten prefix %u, sha256 %{public}s",
               static_cast<unsigned>(pending->length), pending->overwritten, hash);
    }
    IOFree(pending, sizeof(*pending) + pending->length);
}
}  // namespace

extern "C" void switch_wlan_rom_captured(const uint8_t *bytes, size_t length, uint32_t overwrittenPrefix)
{
    if (!bytes || !length || length > 0x1000000 || overwrittenPrefix > length)
        return;
    auto *pending = static_cast<SwitchWLANRom *>(IOMallocZero(sizeof(SwitchWLANRom) + length));
    if (!pending) {
        IOLog("SwitchWLAN: ROM capture could not retain %zu bytes\n", length);
        char hash[65];
        switch_rom_hash(bytes, length, hash);
        switch_rom_log(bytes, length, overwrittenPrefix, hash);
        return;
    }
    pending->length = length;
    pending->overwritten = overwrittenPrefix;
    memcpy(pending + 1, bytes, length);
    if (!switch_wlan_rom_publish(&switch_rom_state, pending)) {
        IOFree(pending, sizeof(*pending) + length);
        return;
    }
    switch_rom_persist();
}

extern "C" void switch_wlan_data_mounted(void)
{
    __atomic_store_n(&switch_rom_state.switch_mounted, true, __ATOMIC_SEQ_CST);
    switch_rom_persist();
}
#endif
