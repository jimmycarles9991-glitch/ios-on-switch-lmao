// SPDX-License-Identifier: GPL-2.0-only
/*
 * <switch/icon_cache.h>. iconservicesagent keeps the icons it rendered while their __cache_info__ names the system's
 * ProductBuildVersion, the folder is owned by _iconservices, and no ACL is present (-[ISMutableIconCache initWithURL:],
 * 21H461 and 24B119). Our GPU build does not change ProductBuildVersion, so icons a GPU build drew outlive it. The
 * deploy names the GPU build it staged in Switch/icon-cache-gpu (tools/deploy/icon_cache.py). When that name differs
 * from the marker Switch/icon-cache-reset, 21H461 unlinks top-level __cache_info__ while 24B119 atomically renames each
 * salt folder aside into Switch/icon-cache-trash/<boot-session>-<salt> so iconservicesagent resets an empty directory
 * off SpringBoard's critical path and the trash is swept after System Boot Complete.
 */
#include <switch/icon_cache.h>

#include <string.h>
#include <sys/errno.h>

#if XNU_12377_121_6
/* 24B119: the cache is in salt folders below this one (-[ISDefaults cacheURL]: a UUID of IconServices' build time, the
 * BuildVersion and the UniqueDeviceID), each with its own __cache_info__. */
#define SWITCH_ICON_CACHE_SALTED 1
static const char switch_icon_cache_folder[] =
    "containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache/Library/Caches/com.apple.IconsCache";
#elif XNU_10063_141_1
/* 21H461: __cache_info__ at this folder's top. */
#define SWITCH_ICON_CACHE_SALTED 0
static const char switch_icon_cache_folder[] =
    "containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache/Library/Caches/com.apple.IconsCache";
#else
#error "the icon cache layout of this XNU's iOS"
#endif

static const char switch_icon_cache_request[] = "Switch/icon-cache-gpu";
static const char switch_icon_cache_marker[] = "Switch/icon-cache-reset";

enum {
    kPath = 256,
    kSweepPath = 512,
    kBuild = 128
};

#if SWITCH_ICON_CACHE_SALTED
static bool switch_icon_cache_valid_name(const char *name)
{
    if (!name || !name[0]) {
        return false;
    }
    if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) {
        return false;
    }
    for (const char *p = name; *p; ++p) {
        if (*p == '/') {
            return false;
        }
    }
    return true;
}

struct switch_icon_cache_walk {
    const struct switch_icon_cache_ops *ops;
    struct switch_icon_cache_result *result;
    bool trash_ready;
};

static int switch_icon_cache_move_salt(void *walk, const char *salt)
{
    struct switch_icon_cache_walk *each = walk;
    const struct switch_icon_cache_ops *ops = each->ops;
    char from[kPath], to[kPath];

    if (!ops->mkdir || !ops->rename || !switch_icon_cache_valid_name(salt) ||
        !switch_icon_cache_valid_name(ops->session)) {
        return EINVAL;
    }
    const size_t folder = strlen(switch_icon_cache_folder);
    const size_t salt_len = strlen(salt);
    const size_t trash = strlen(SWITCH_ICON_CACHE_TRASH);
    const size_t session_len = strlen(ops->session);
    if (folder + 1 + salt_len + 1 > sizeof(from) || trash + 1 + session_len + 1 + salt_len + 1 > sizeof(to)) {
        return ENAMETOOLONG;
    }

    memcpy(from, switch_icon_cache_folder, folder);
    from[folder] = '/';
    memcpy(from + folder + 1, salt, salt_len + 1);

    memcpy(to, SWITCH_ICON_CACHE_TRASH, trash);
    to[trash] = '/';
    memcpy(to + trash + 1, ops->session, session_len);
    to[trash + 1 + session_len] = '-';
    memcpy(to + trash + 1 + session_len + 1, salt, salt_len + 1);

    if (!each->trash_ready) {
        const int mk_error = ops->mkdir(ops->context, SWITCH_ICON_CACHE_TRASH, 0700);
        if (mk_error) {
            return mk_error;
        }
        each->trash_ready = true;
    }

    const int error = ops->rename(ops->context, from, to);
    if (error == ENOENT) {
        return 0;
    }
    if (error) {
        return error;
    }
    ++each->result->dropped;
    each->result->trash_pending = true;
    return 0;
}

static int switch_icon_cache_clear(const struct switch_icon_cache_ops *ops, struct switch_icon_cache_result *result)
{
    struct switch_icon_cache_walk walk = {ops, result, false};
    const int error = ops->folders(ops->context, switch_icon_cache_folder, switch_icon_cache_move_salt, &walk);
    return error == ENOENT ? 0 : error;
}
#else
/* __cache_info__ at the cache folder's top; none there is no error. */
static int switch_icon_cache_unlink(const struct switch_icon_cache_ops *ops, struct switch_icon_cache_result *result)
{
    static const char info[] = "/__cache_info__";
    char path[kPath];
    const size_t folder = strlen(switch_icon_cache_folder);
    if (folder + sizeof(info) > sizeof(path)) {
        return ENAMETOOLONG;
    }
    memcpy(path, switch_icon_cache_folder, folder);
    memcpy(path + folder, info, sizeof(info));
    const int error = ops->unlink(ops->context, path);
    if (!error) {
        ++result->dropped;
    }
    return error == ENOENT ? 0 : error;
}

static int switch_icon_cache_clear(const struct switch_icon_cache_ops *ops, struct switch_icon_cache_result *result)
{
    const int error = switch_icon_cache_unlink(ops, result);
    return error == ENOENT ? 0 : error;
}
#endif

void switch_icon_cache_drop(const struct switch_icon_cache_ops *ops, struct switch_icon_cache_result *result)
{
    char build[kBuild];
    size_t length = 0;
    memset(result, 0, sizeof(*result));
    int error = ops->read(ops->context, switch_icon_cache_request, build, sizeof(build), &length);
    if (error != ENOENT && (error || length) &&
        (error || !ops->holds(ops->context, switch_icon_cache_marker, build, length))) {
        result->requested = true;
        for (size_t i = 0; !error && i < length && i < sizeof(result->build) - 1 && build[i] != '\n'; ++i) {
            result->build[i] = build[i];
        }
        if (!error) {
            error = switch_icon_cache_clear(ops, result);
        }
        if (!error) {
            error = ops->write(ops->context, switch_icon_cache_marker, build, length);
        }
        result->error = error;
    }
#if SWITCH_ICON_CACHE_SALTED
    if (!result->trash_pending && ops->exists) {
        result->trash_pending = ops->exists(ops->context, SWITCH_ICON_CACHE_TRASH);
    }
#endif
}

bool switch_icon_cache_state_step(uint32_t current, uint32_t event_bit, uint32_t *next)
{
    uint32_t desired = current | event_bit;
    const bool start = (desired & SWITCH_ICON_CACHE_STATE_PENDING) != 0 &&
                       (desired & SWITCH_ICON_CACHE_STATE_BOOT_DONE) != 0 &&
                       (desired & SWITCH_ICON_CACHE_STATE_STARTED) == 0;
    if (start) {
        desired |= SWITCH_ICON_CACHE_STATE_STARTED;
    }
    *next = desired;
    return start;
}

#if SWITCH_ICON_CACHE_SALTED
static int switch_icon_cache_sweep_dir(const struct switch_icon_cache_sweep_ops *ops,
                                       struct switch_icon_cache_sweep_result *result, char path[kSweepPath],
                                       size_t path_len, unsigned int depth, unsigned int *steps)
{
    char *const names = ops->scratch + (size_t)depth * ops->scratch_stride;
    const char *const names_end = names + ops->scratch_stride;

    for (;;) {
        size_t count = 0;
        int error = ops->read_batch(ops->context, path, names, ops->scratch_stride, &count);
        if (error == ENOENT) {
            return 0;
        }
        if (error) {
            return error;
        }
        if (!count) {
            break;
        }

        const char *cursor = names;
        for (size_t i = 0; i < count; ++i) {
            if (++(*steps) > SWITCH_ICON_CACHE_SWEEP_MAX_ENTRIES) {
                return ELOOP;
            }
            if (cursor >= names_end) {
                return EINVAL;
            }
            const size_t avail = (size_t)(names_end - cursor);
            const size_t name_len = strnlen(cursor, avail);
            if (name_len == avail || !switch_icon_cache_valid_name(cursor)) {
                return EINVAL;
            }
            if (path_len + 1 + name_len + 1 > kSweepPath) {
                return ENAMETOOLONG;
            }

            path[path_len] = '/';
            memcpy(path + path_len + 1, cursor, name_len + 1);
            const size_t child_len = path_len + 1 + name_len;
            cursor += name_len + 1;

            enum switch_icon_cache_entry_kind kind = SWITCH_ICON_CACHE_ENTRY_MISSING;
            error = ops->inspect(ops->context, path, &kind);
            if (error == ENOENT || (!error && kind == SWITCH_ICON_CACHE_ENTRY_MISSING)) {
                path[path_len] = '\0';
                continue;
            }
            if (error) {
                path[path_len] = '\0';
                return error;
            }

            switch (kind) {
            case SWITCH_ICON_CACHE_ENTRY_SYMLINK:
                path[path_len] = '\0';
                return ELOOP;
            case SWITCH_ICON_CACHE_ENTRY_FOREIGN:
                path[path_len] = '\0';
                return EXDEV;
            case SWITCH_ICON_CACHE_ENTRY_REG:
                error = ops->unlink(ops->context, path);
                if (!error) {
                    ++result->files_removed;
                } else if (error != ENOENT) {
                    path[path_len] = '\0';
                    return error;
                }
                break;
            case SWITCH_ICON_CACHE_ENTRY_DIR:
                if (depth >= SWITCH_ICON_CACHE_SWEEP_MAX_DEPTH) {
                    path[path_len] = '\0';
                    return ELOOP;
                }
                error = switch_icon_cache_sweep_dir(ops, result, path, child_len, depth + 1, steps);
                if (error) {
                    path[path_len] = '\0';
                    return error;
                }
                break;
            case SWITCH_ICON_CACHE_ENTRY_OTHER:
            default:
                path[path_len] = '\0';
                return EPERM;
            }
            path[path_len] = '\0';
        }
    }

    const int error = ops->rmdir(ops->context, path);
    if (!error) {
        ++result->dirs_removed;
        return 0;
    }
    return error == ENOENT ? 0 : error;
}

void switch_icon_cache_sweep(const struct switch_icon_cache_sweep_ops *ops,
                             struct switch_icon_cache_sweep_result *result)
{
    char path[kSweepPath];
    enum switch_icon_cache_entry_kind root_kind = SWITCH_ICON_CACHE_ENTRY_MISSING;
    unsigned int steps = 0;

    memset(result, 0, sizeof(*result));
    if (!ops || !ops->scratch || !ops->scratch_stride || !ops->read_batch || !ops->inspect || !ops->unlink ||
        !ops->rmdir) {
        result->error = EINVAL;
        return;
    }

    const int error = ops->inspect(ops->context, SWITCH_ICON_CACHE_TRASH, &root_kind);
    if (error == ENOENT || (!error && root_kind == SWITCH_ICON_CACHE_ENTRY_MISSING)) {
        return;
    }
    if (error) {
        result->error = error;
        return;
    }
    if (root_kind == SWITCH_ICON_CACHE_ENTRY_SYMLINK) {
        result->error = ELOOP;
        return;
    }
    if (root_kind == SWITCH_ICON_CACHE_ENTRY_FOREIGN) {
        result->error = EXDEV;
        return;
    }
    if (root_kind != SWITCH_ICON_CACHE_ENTRY_DIR) {
        result->error = ENOTDIR;
        return;
    }

    const size_t root_len = strlen(SWITCH_ICON_CACHE_TRASH);
    memcpy(path, SWITCH_ICON_CACHE_TRASH, root_len + 1);
    result->error = switch_icon_cache_sweep_dir(ops, result, path, root_len, 0, &steps);
}
#endif
