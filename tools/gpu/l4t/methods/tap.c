#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define __packed __attribute__((packed))
#include "nvgpu.h"

struct switch_mapping {
    ino_t as;
    uint64_t va, length, file_offset;
    int storage;
};
static struct switch_mapping switch_maps[4096];

static struct {
    ino_t channel, as;
} switch_bindings[64];

static unsigned switch_map_count, switch_binding_count, switch_sequence;
static pthread_mutex_t switch_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool switch_app_submit;

void switch_mark_draw_submit(void)
{
    atomic_store(&switch_app_submit, 1);
}

static ino_t switch_inode(int fd)
{
    struct stat st;
    if (fstat(fd, &st))
        abort();
    return st.st_ino;
}

static void switch_forget_mapping(ino_t as, uint64_t va)
{
    for (unsigned i = 0; i < switch_map_count; i++) {
        if (switch_maps[i].as == as && switch_maps[i].va == va) {
            close(switch_maps[i].storage);
            switch_maps[i] = switch_maps[--switch_map_count];
            return;
        }
    }
}

static void switch_remember_mapping(ino_t as, const struct nvgpu_as_map_buffer_ex_args *args)
{
    struct stat st;
    if (fstat(args->dmabuf_fd, &st) || st.st_size < 0 || args->buffer_offset > (uint64_t)st.st_size)
        abort();
    uint64_t available = (uint64_t)st.st_size - args->buffer_offset;
    uint64_t length = args->mapping_size ? args->mapping_size : available;
    switch_forget_mapping(as, args->offset);
    if (switch_map_count == sizeof(switch_maps) / sizeof(switch_maps[0]))
        abort();
    int fd = fcntl(args->dmabuf_fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0)
        abort();
    switch_maps[switch_map_count++] = (struct switch_mapping){as, args->offset, length, args->buffer_offset, fd};
}

static void switch_bind_channel(ino_t as, ino_t channel)
{
    for (unsigned i = 0; i < switch_binding_count; i++) {
        if (switch_bindings[i].channel == channel) {
            switch_bindings[i].as = as;
            return;
        }
    }
    if (switch_binding_count == sizeof(switch_bindings) / sizeof(switch_bindings[0]))
        abort();
    switch_bindings[switch_binding_count].channel = channel;
    switch_bindings[switch_binding_count++].as = as;
}

static void switch_snapshot_entry(ino_t as, uint64_t va, size_t size, unsigned entry)
{
    for (unsigned i = 0; i < switch_map_count; i++) {
        const struct switch_mapping *map = &switch_maps[i];
        if (as != map->as || va < map->va || va - map->va > map->length || size > map->length - (va - map->va))
            continue;
        char name[64];
        snprintf(name, sizeof(name), "push-%04u-%03u.bin", switch_sequence, entry);
        FILE *out = fopen(name, "wbx");
        if (!out)
            abort();
        uint64_t offset = map->file_offset + (va - map->va);
        while (size) {
            char buffer[16384];
            ssize_t n = pread(map->storage, buffer, size < sizeof(buffer) ? size : sizeof(buffer), (off_t)offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0 || fwrite(buffer, 1, n, out) != (size_t)n)
                abort();
            offset += n;
            size -= n;
        }
        if (fclose(out))
            abort();
        return;
    }
    fprintf(stderr, "unmapped GPFIFO address %#llx (%zu bytes)\n", (unsigned long long)va, size);
    abort();
}

static int switch_capture_submit(int fd, struct nvgpu_submit_gpfifo_args *submit)
{
    const struct nvgpu_gpfifo *entries = (void *)(uintptr_t)submit->gpfifo;
    ino_t channel = switch_inode(fd), as = 0;
    for (unsigned i = 0; i < switch_binding_count; i++) {
        if (switch_bindings[i].channel == channel)
            as = switch_bindings[i].as;
    }
    if (!as)
        abort();
    FILE *log = fopen("submit.tsv", "a");
    if (!log)
        abort();
    fprintf(log, "submit\t%u\t%lu\t%u\t%u\t%d\n", switch_sequence, (unsigned long)channel, submit->flags,
            submit->num_entries, atomic_load(&switch_app_submit));
    for (unsigned j = 0; j < submit->num_entries; j++) {
        uint32_t lo = entries[j].entry0, hi = entries[j].entry1;
        uint64_t va = (uint64_t)(hi & 255) << 32 | (lo & ~3u);
        size_t bytes = (size_t)((hi >> 10) & 0x1fffff) * 4;
        fprintf(log, "entry\t%u\t%u\t%llx\t%zu\t%08x\t%08x\n", switch_sequence, j, (unsigned long long)va, bytes, lo,
                hi);
        if (bytes)
            switch_snapshot_entry(as, va, bytes, j);
    }
    if (fclose(log))
        abort();
    switch_sequence++;
    if (submit->flags & NVGPU_SUBMIT_GPFIFO_FLAGS_FENCE_GET) {
        submit->fence.id = submit->flags & NVGPU_SUBMIT_GPFIFO_FLAGS_SYNC_FENCE ? eventfd(1, EFD_CLOEXEC) : 1;
        if ((int32_t)submit->fence.id < 0)
            return -1;
        submit->fence.value = switch_sequence;
    }
    return 0;
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *args = va_arg(ap, void *);
    va_end(ap);
    int (*next)(int, unsigned long, ...) = dlsym(RTLD_NEXT, "ioctl");
    uint32_t cmd = (uint32_t)request;
    if (cmd != NVGPU_IOCTL_CHANNEL_SUBMIT_GPFIFO && cmd != NVGPU_AS_IOCTL_BIND_CHANNEL &&
        cmd != NVGPU_AS_IOCTL_MAP_BUFFER_EX && cmd != NVGPU_AS_IOCTL_UNMAP_BUFFER)
        return next(fd, request, args);
    pthread_mutex_lock(&switch_lock);
    int result;
    if (cmd == NVGPU_IOCTL_CHANNEL_SUBMIT_GPFIFO) {
        result = switch_capture_submit(fd, args);
    } else {
        result = next(fd, request, args);
        if (result == 0) {
            ino_t as = switch_inode(fd);
            if (cmd == NVGPU_AS_IOCTL_BIND_CHANNEL)
                switch_bind_channel(as, switch_inode(((const struct nvgpu_as_bind_channel_args *)args)->channel_fd));
            else if (cmd == NVGPU_AS_IOCTL_MAP_BUFFER_EX)
                switch_remember_mapping(as, args);
            else
                switch_forget_mapping(as, ((const struct nvgpu_as_unmap_buffer_args *)args)->offset);
        }
    }
    int saved = errno;
    pthread_mutex_unlock(&switch_lock);
    errno = saved;
    return result;
}
