#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#define __packed __attribute__((packed))
#include "nvgpu.h"
#include "nvmap.h"

static const char *mapped_path(const char *path)
{
    static const char prefix[] = "/sys/module/tegra_fuse/parameters/";
    if (!strncmp(path, prefix, sizeof(prefix) - 1)) {
        const char *name = path + sizeof(prefix) - 1;
        if (!strcmp(name, "tegra_chip_id"))
            return "/work/fuse-id";
        if (!strcmp(name, "tegra_chip_rev"))
            return "/work/fuse-rev";
        if (!strcmp(name, "tegra_platform"))
            return "/work/fuse-platform";
    }
    return path;
}

static int open_path(const char *path, int flags, va_list args)
{
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE)
        mode = va_arg(args, int);
    return syscall(SYS_openat, AT_FDCWD, mapped_path(path), flags, mode);
}

int open(const char *path, int flags, ...)
{
    va_list args;
    va_start(args, flags);
    int fd = open_path(path, flags, args);
    va_end(args);
    return fd;
}

int open64(const char *path, int flags, ...)
{
    va_list args;
    va_start(args, flags);
    int fd = open_path(path, flags, args);
    va_end(args);
    return fd;
}

FILE *fopen(const char *path, const char *mode)
{
    FILE *(*real)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
    return real(mapped_path(path), mode);
}

FILE *fopen64(const char *path, const char *mode)
{
    return fopen(path, mode);
}

static const struct nvgpu_gpu_characteristics chip = {
    .arch = NVGPU_GPU_ARCH_GM200,
    .impl = NVGPU_GPU_IMPL_GM20B,
    .rev = 0xa1,
    .num_gpc = 1,
    .L2_cache_size = 256 * 1024,
    .num_tpc_per_gpc = 2,
    .bus_type = NVGPU_GPU_BUS_TYPE_AXI,
    .big_page_size = 65536,
    .compression_page_size = 131072,
    .pde_coverage_bit_count = 27,
    .flags = NVGPU_GPU_FLAGS_SUPPORT_MAP_DIRECT_KIND_CTRL | NVGPU_GPU_FLAGS_HAS_SYNCPOINTS |
             NVGPU_GPU_FLAGS_SUPPORT_PARTIAL_MAPPINGS | NVGPU_GPU_FLAGS_SUPPORT_SPARSE_ALLOCS |
             NVGPU_GPU_FLAGS_SUPPORT_SYNC_FENCE_FDS | NVGPU_GPU_FLAGS_SUPPORT_USERSPACE_MANAGED_AS |
             NVGPU_GPU_FLAGS_SUPPORT_TSG,
    .available_big_page_sizes = 65536 | 131072,
    .twod_class = 0x902d,
    .threed_class = 0xb197,
    .compute_class = 0xb1c0,
    .gpfifo_class = 0xb06f,
    .inline_to_memory_class = 0xa140,
    .dma_copy_class = 0xb0b5,
    .gpc_mask = 1,
    .sm_arch_sm_version = 0x503,
    .sm_arch_spa_version = 0x503,
    .sm_arch_warp_count = 64,
    .gpu_va_bit_count = 40,
    .gpu_ioctl_nr_last = NVGPU_GPU_IOCTL_LAST,
    .tsg_ioctl_nr_last = NVGPU_TSG_IOCTL_LAST,
    .dbg_gpu_ioctl_nr_last = NVGPU_DBG_GPU_IOCTL_LAST,
    .ioctl_channel_nr_last = NVGPU_IOCTL_CHANNEL_LAST,
    .as_ioctl_nr_last = NVGPU_AS_IOCTL_LAST,
    .max_fbps_count = 1,
    .fbp_en_mask = 1,
    .max_ltc_per_fbp = 2,
    .max_lts_per_ltc = 1,
    .max_tex_per_tpc = 4,
    .max_gpc_count = 1,
    .rop_l2_en_mask_DEPRECATED = {3, 0},
    .chipname = "gm20b",
    .num_ltc = 2,
    .lts_per_ltc = 1,
    .max_freq = 768000000,
    .graphics_preemption_mode_flags = 1,
    .compute_preemption_mode_flags = 1,
    .default_graphics_preempt_mode = 1,
    .default_compute_preempt_mode = 1};

int ioctl(int fd, unsigned long cmd, ...)
{
    va_list args;
    va_start(args, cmd);
    void *arg = va_arg(args, void *);
    va_end(args);
    char path[64], name[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t len = readlink(path, name, sizeof(name) - 1);
    if (len < 0)
        return syscall(SYS_ioctl, fd, cmd, arg);
    name[len] = 0;
    if (strcmp(name, "/dev/nvmap") && strcmp(name, "/dev/nvhost-ctrl-gpu") && strcmp(name, "/dev/nvhost-ctrl") &&
        strcmp(name, "/dev/nvhost-gpu") && strcmp(name, "/dev/nvhost-as-gpu") && strncmp(name, "/memfd:switch-", 14))
        return syscall(SYS_ioctl, fd, cmd, arg);

    switch ((uint32_t)cmd) {
    case NVGPU_GPU_IOCTL_GET_CHARACTERISTICS: {
        struct nvgpu_gpu_get_characteristics *q = arg;
        size_t n = q->gpu_characteristics_buf_size;

        if (n)
            memcpy((void *)(uintptr_t)q->gpu_characteristics_buf_addr, &chip, n < sizeof(chip) ? n : sizeof(chip));
        q->gpu_characteristics_buf_size = sizeof(chip);
        return 0;
    }
    case NVGPU_GPU_IOCTL_ALLOC_AS: {
        struct nvgpu_alloc_as_args *q = arg;
        q->as_fd = memfd_create("switch-as", MFD_CLOEXEC);
        return q->as_fd < 0 ? -1 : 0;
    }
    case NVGPU_AS_IOCTL_GET_VA_REGIONS: {
        struct nvgpu_as_get_va_regions_args *q = arg;
        const struct nvgpu_as_va_region regions[] = {
            {.offset = 1ULL << 26, .page_size = 4096, .pages = ((1ULL << 34) - (1ULL << 26)) / 4096},
            {.offset = 1ULL << 34, .page_size = 65536, .pages = ((1ULL << 37) - (1ULL << 34)) / 65536}};
        if (q->buf_size && q->buf_addr)
            memcpy((void *)(uintptr_t)q->buf_addr, regions,
                   q->buf_size < sizeof(regions) ? q->buf_size : sizeof(regions));
        q->buf_size = sizeof(regions);
        return 0;
    }
    case NVGPU_GPU_IOCTL_OPEN_TSG:
        ((struct nvgpu_gpu_open_tsg_args *)arg)->tsg_fd = memfd_create("switch-tsg", MFD_CLOEXEC);
        return ((struct nvgpu_gpu_open_tsg_args *)arg)->tsg_fd < 0 ? -1 : 0;
    case NVGPU_GPU_IOCTL_OPEN_CHANNEL:
        ((struct nvgpu_gpu_open_channel_args *)arg)->out.channel_fd = memfd_create("switch-channel", MFD_CLOEXEC);
        return ((struct nvgpu_gpu_open_channel_args *)arg)->out.channel_fd < 0 ? -1 : 0;
    case NVGPU_AS_IOCTL_BIND_CHANNEL:
    case NVGPU_AS_IOCTL_FREE_SPACE:
    case NVGPU_AS_IOCTL_UNMAP_BUFFER:
    case NVGPU_TSG_IOCTL_BIND_CHANNEL:
        return 0;
    case NVMAP_IOC_CREATE:
    case NVMAP_IOC_CREATE_64: {
        struct nvmap_create_handle *q = arg;
        size_t size = (uint32_t)cmd == NVMAP_IOC_CREATE ? q->size : q->size64;
        int storage = memfd_create("switch-buffer", MFD_CLOEXEC);
        if (storage < 0)
            return -1;
        if (ftruncate(storage, size)) {
            int saved = errno;
            close(storage);
            errno = saved;
            return -1;
        }
        if ((uint32_t)cmd == NVMAP_IOC_CREATE)
            q->handle = storage;
        else
            q->handle64 = storage;
        return 0;
    }
    case NVMAP_IOC_ALLOC:
    case NVMAP_IOC_ALLOC_KIND:
        return 0;
    case NVMAP_IOC_GET_FD: {
        struct nvmap_create_handle *q = arg;
        q->fd = dup(q->handle);
        return q->fd < 0 ? -1 : 0;
    }
    case NVMAP_IOC_FREE:
        return close((int)(uintptr_t)arg);
    case NVGPU_GPU_IOCTL_GET_GPU_TIME: {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts))
            return -1;
        ((struct nvgpu_gpu_get_gpu_time_args *)arg)->gpu_timestamp = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
        return 0;
    }
    case NVGPU_AS_IOCTL_MAP_BUFFER_EX: {
        struct nvgpu_as_map_buffer_ex_args *q = arg;
        static uint64_t next_page = 0x500000000ULL;
        if (!q->page_size)
            q->page_size = 4096;
        if (!(q->flags & NVGPU_AS_MAP_BUFFER_FLAGS_FIXED_OFFSET)) {
            uint64_t size = q->mapping_size;
            if (!size) {
                off_t end = lseek(q->dmabuf_fd, 0, SEEK_END);
                if (end <= 0)
                    return -1;
                size = end;
            }
            next_page = (next_page + 0x1ffff) & ~0x1ffffULL;
            q->offset = next_page;
            next_page += size;
        }
        return 0;
    }
    case NVGPU_AS_IOCTL_ALLOC_SPACE: {
        struct nvgpu_as_alloc_space_args *q = arg;
        static uint64_t next_va = 0x800000000ULL;
        if (!(q->flags & NVGPU_AS_ALLOC_SPACE_FLAGS_FIXED_OFFSET)) {
            uint64_t alignment = q->o_a.align ? q->o_a.align : q->page_size;
            next_va = (next_va + alignment - 1) & -alignment;
            q->o_a.offset = next_va;
            next_va += (uint64_t)q->pages * q->page_size;
        }
        return 0;
    }
    case NVGPU_IOCTL_CHANNEL_ALLOC_OBJ_CTX: {
        struct nvgpu_alloc_obj_ctx_args *q = arg;
        q->obj_id = q->class_num;
        return 0;
    }
    case NVGPU_IOCTL_CHANNEL_ALLOC_GPFIFO_EX:
    case NVGPU_IOCTL_CHANNEL_SET_ERROR_NOTIFIER:
    case NVGPU_IOCTL_CHANNEL_ZCULL_BIND:
    case NVGPU_IOCTL_CHANNEL_WDT:
    case NVGPU_TSG_IOCTL_UNBIND_CHANNEL:
        return 0;
    case NVGPU_GPU_IOCTL_GET_ENGINE_INFO: {
        struct nvgpu_gpu_get_engine_info_args *q = arg;
        const struct nvgpu_gpu_get_engine_info_item engines[] = {{.engine_id = 0, .runlist_id = 0},
                                                                 {.engine_id = 1, .runlist_id = 0}};
        if (q->engine_info_buf_size && q->engine_info_buf_addr)
            memcpy((void *)(uintptr_t)q->engine_info_buf_addr, engines,
                   q->engine_info_buf_size < sizeof(engines) ? q->engine_info_buf_size : sizeof(engines));
        q->engine_info_buf_size = sizeof(engines);
        return 0;
    }
    case NVGPU_GPU_IOCTL_NUM_VSMS:
        ((struct nvgpu_gpu_num_vsms *)arg)->num_vsms = 2;
        return 0;
    case NVGPU_GPU_IOCTL_VSMS_MAPPING: {
        struct nvgpu_gpu_vsms_mapping *q = arg;
        struct nvgpu_gpu_vsms_mapping_entry sms[] = {{0, 0}, {0, 1}};
        memcpy((void *)(uintptr_t)q->vsms_map_buf_addr, sms, sizeof(sms));
        return 0;
    }
    case NVGPU_GPU_IOCTL_ZCULL_GET_CTX_SIZE:
        ((struct nvgpu_gpu_zcull_get_ctx_size_args *)arg)->size = 0x400;
        return 0;
    case NVGPU_GPU_IOCTL_ZCULL_GET_INFO:
        *(struct nvgpu_gpu_zcull_get_info_args *)arg =
            (struct nvgpu_gpu_zcull_get_info_args){32, 32, 1024, 2048, 32, 32, 192, 32, 64, 16};
        return 0;
    case NVMAP_IOC_GET_AVAILABLE_HEAPS:
        ((struct nvmap_available_heaps *)arg)->heaps = 0x40000000;
        return 0;
    case NVGPU_GPU_IOCTL_GET_FBP_L2_MASKS:
    case NVGPU_GPU_IOCTL_GET_TPC_MASKS: {
        struct nvgpu_gpu_get_tpc_masks_args *q = arg;
        if (q->mask_buf_size >= 4)
            *(uint32_t *)(uintptr_t)q->mask_buf_addr = 3;
        q->mask_buf_size = 4;
        return 0;
    }
    default:
        errno = ENOTTY;
        return -1;
    }
}
