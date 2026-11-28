// SPDX-License-Identifier: GPL-2.0-only
/*
 * <switch/icon_cache.h>. iconservicesagent keeps the icons it rendered while their __cache_info__ names the system's
 * ProductBuildVersion, the folder is owned by _iconservices, and no ACL is present (-[ISMutableIconCache initWithURL:],
 * 21H461 and 24B119). Our GPU build does not change ProductBuildVersion, so icons a GPU build drew outlive it. The
 * GPU stager names the build it staged in Switch/icon-cache-gpu (stage-tegra-gm20b-metal.py, which the deploy and the
 * image builder both run). When that name differs
 * from the marker Switch/icon-cache-reset, 21H461 unlinks top-level __cache_info__. 24B119 renames each salt folder
 * aside, off SpringBoard's critical path: into Switch/icon-cache-keep/<marker's build> when the marker names one, else
 * into Switch/icon-cache-trash/<boot-session>-<salt>; and when the keep holds the named build's folders (the card went
 * back to a build it ran before, as sessions sharing it restage each other's), they move back into the cache, which
 * then holds icons that build drew and needs no reset (Switch/icon-cache-restored names it). The keep holds one build;
 * anything else in it, and the trash, are swept after System Boot Complete.
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
#if SWITCH_ICON_CACHE_SALTED
/* The request a drop began for, written before its first rename. Unlike the marker it is written whether or not the
 * drop finishes, so a marker that does not hold it means a drop began and stopped: the cache may then hold folders of
 * any build (moved back for the request, or drawn by the agent on the boot after), and none of it can be kept as the
 * marker's. */
static const char switch_icon_cache_attempt[] = "Switch/icon-cache-attempt";
#endif
#if DEVELOPMENT || DEBUG
/* Written, with the named build, on a boot that moved that build's kept folders back (HW Test system.icon_cache_reset:
 * such a drop is not followed by the agent's reset). HW Test ships in debug packages only. */
static const char switch_icon_cache_restored[] = "Switch/icon-cache-restored";
#endif

enum {
    kPath = 256,
    kSweepPath = 512,
    kBuild = 128,
    kBuildKey = 13
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

/* Moves each visited folder from `from` into `to`, or into the trash as <session>-<prefix><name> when `to` is NULL; a
 * folder named `stay` stays. */
struct switch_icon_cache_walk {
    const struct switch_icon_cache_ops *ops;
    struct switch_icon_cache_result *result;
    const char *from;
    const char *to;
    const char *prefix;
    const char *stay;
    int moved;
    bool trash_ready;
};

static int switch_icon_cache_join(char out[kPath], const char *a, const char *b, const char *c)
{
    const size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    if (la + lb + lc + 1 > kPath) {
        return ENAMETOOLONG;
    }
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    memcpy(out + la + lb, c, lc + 1);
    return 0;
}

static int switch_icon_cache_move(void *opaque, const char *name)
{
    struct switch_icon_cache_walk *walk = opaque;
    const struct switch_icon_cache_ops *ops = walk->ops;
    char from[kPath], to[kPath], dir[kPath];

    if (!ops->mkdir || !ops->rename || !switch_icon_cache_valid_name(name)) {
        return EINVAL;
    }
    if (walk->stay && !strcmp(name, walk->stay)) {
        return 0;
    }
    int error = switch_icon_cache_join(dir, walk->from, "/", "");
    if (!error) {
        error = switch_icon_cache_join(from, dir, name, "");
    }
    if (!error && walk->to) {
        error = switch_icon_cache_join(dir, walk->to, "/", "");
        if (!error) {
            error = switch_icon_cache_join(to, dir, name, "");
        }
    } else if (!error) {
        if (!switch_icon_cache_valid_name(ops->session)) {
            return EINVAL;
        }
        error = switch_icon_cache_join(dir, SWITCH_ICON_CACHE_TRASH "/", ops->session, "-");
        if (!error) {
            error = switch_icon_cache_join(to, dir, walk->prefix, name);
        }
        if (!error && !walk->trash_ready) {
            error = ops->mkdir(ops->context, SWITCH_ICON_CACHE_TRASH, 0700);
            walk->trash_ready = !error;
        }
    }
    if (error) {
        return error;
    }
    error = ops->rename(ops->context, from, to);
    if (error == ENOENT) {
        return 0;
    }
    if (error) {
        return error;
    }
    ++walk->moved;
    if (!walk->to) {
        walk->result->trash_pending = true;
    }
    return 0;
}

static int switch_icon_cache_walk_from(struct switch_icon_cache_walk *walk, const char *from, const char *to,
                                       const char *prefix, const char *stay)
{
    walk->from = from;
    walk->to = to;
    walk->prefix = prefix;
    walk->stay = stay;
    walk->moved = 0;
    const int error = walk->ops->folders(walk->ops->context, from, switch_icon_cache_move, walk);
    return error == ENOENT ? 0 : error;
}

/* Moves a kept build's folder to the trash: its salt folders one by one, as <session>-keep-<build>-<salt>, so the sweep
 * meets them at the depth it meets any salt folder, then the emptied folder. The build named `stay` stays. */
struct switch_icon_cache_evict {
    struct switch_icon_cache_walk *walk;
    const char *stay;
};

static int switch_icon_cache_evict_build(void *opaque, const char *name)
{
    struct switch_icon_cache_evict *evict = opaque;
    struct switch_icon_cache_walk *walk = evict->walk;
    struct switch_icon_cache_walk salts = {.ops = walk->ops, .result = walk->result};
    char dir[kPath], prefix[kPath];

    if (!switch_icon_cache_valid_name(name)) {
        return EINVAL;
    }
    if (evict->stay && !strcmp(name, evict->stay)) {
        return 0;
    }
    int error = switch_icon_cache_join(dir, SWITCH_ICON_CACHE_KEEP "/", name, "");
    if (!error) {
        error = switch_icon_cache_join(prefix, "keep-", name, "-");
    }
    if (!error) {
        error = switch_icon_cache_walk_from(&salts, dir, NULL, prefix, NULL);
    }
    if (!error) {
        walk->from = SWITCH_ICON_CACHE_KEEP;
        walk->to = NULL;
        walk->prefix = "keep-";
        walk->stay = NULL;
        error = switch_icon_cache_move(walk, name);
    }
    return error;
}

static int switch_icon_cache_evict_keep(struct switch_icon_cache_walk *walk, const char *stay)
{
    struct switch_icon_cache_evict evict = {walk, stay};
    const int error =
        walk->ops->folders(walk->ops->context, SWITCH_ICON_CACHE_KEEP, switch_icon_cache_evict_build, &evict);
    return error == ENOENT ? 0 : error;
}

/* `kept_for`: the build the cache's icons were drawn for (the marker's), or NULL; `wanted`: the build the deploy named,
 * or NULL. Order: the keep loses every build but `wanted`, the cache's folders go into the keep for `kept_for` (or the
 * trash), then `wanted`'s come back. Cut short at any step, the marker stays and the next boot runs it again; the
 * worst it loses is the keep, never a folder the agent would read in the wrong place. */
static int switch_icon_cache_clear(const struct switch_icon_cache_ops *ops, struct switch_icon_cache_result *result,
                                   const char *kept_for, const char *wanted)
{
    struct switch_icon_cache_walk walk = {.ops = ops, .result = result};
    char kept[kPath], back[kPath];

    if (!ops->mkdir || !ops->exists) {
        return EINVAL;
    }
    if (wanted && !switch_icon_cache_valid_name(wanted)) {
        wanted = NULL;
    }
    if (kept_for && (!switch_icon_cache_valid_name(kept_for) || (wanted && !strcmp(kept_for, wanted)))) {
        kept_for = NULL;
    }
    int error = 0;
    if (ops->exists(ops->context, SWITCH_ICON_CACHE_KEEP)) {
        error = switch_icon_cache_evict_keep(&walk, wanted);
    }
    if (!error && kept_for) {
        error = switch_icon_cache_join(kept, SWITCH_ICON_CACHE_KEEP "/", kept_for, "");
        if (!error) {
            error = ops->mkdir(ops->context, SWITCH_ICON_CACHE_KEEP, 0700);
        }
        if (!error) {
            error = ops->mkdir(ops->context, kept, 0700);
        }
        if (!error) {
            error = switch_icon_cache_walk_from(&walk, switch_icon_cache_folder, kept, "", NULL);
            result->kept = walk.moved;
        }
    } else if (!error) {
        error = switch_icon_cache_walk_from(&walk, switch_icon_cache_folder, NULL, "", NULL);
        result->dropped = walk.moved;
    }
    if (!error && wanted) {
        error = switch_icon_cache_join(back, SWITCH_ICON_CACHE_KEEP "/", wanted, "");
        if (!error && ops->exists(ops->context, back)) {
            error = switch_icon_cache_walk_from(&walk, back, switch_icon_cache_folder, "", NULL);
            result->restored = walk.moved;
            if (!error) {
                error = switch_icon_cache_evict_keep(&walk, kept_for);
            }
        }
    }
    return error;
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

static int switch_icon_cache_clear(const struct switch_icon_cache_ops *ops, struct switch_icon_cache_result *result,
                                   const char *kept_for, const char *wanted)
{
    (void)kept_for;
    (void)wanted;
    const int error = switch_icon_cache_unlink(ops, result);
    return error == ENOENT ? 0 : error;
}
#endif

/* The first characters of a record's first line, as a folder name; false when there is none. */
static bool switch_icon_cache_key(const char *bytes, size_t length, char key[kBuildKey])
{
    size_t n = 0;
    while (n < length && n < kBuildKey - 1 && bytes[n] != '\n') {
        key[n] = bytes[n];
        ++n;
    }
    key[n] = '\0';
    return n > 0;
}

/* Whether a drop began and stopped: an attempt record the marker does not hold (switch_icon_cache_attempt). */
static bool switch_icon_cache_stopped(const struct switch_icon_cache_ops *ops)
{
#if SWITCH_ICON_CACHE_SALTED
    char attempt[kBuild];
    size_t length = 0;
    return !ops->read(ops->context, switch_icon_cache_attempt, attempt, sizeof(attempt), &length) &&
           !ops->holds(ops->context, switch_icon_cache_marker, attempt, length);
#else
    (void)ops;
    return false;
#endif
}

void switch_icon_cache_drop(const struct switch_icon_cache_ops *ops, struct switch_icon_cache_result *result)
{
    char build[kBuild], marker[kBuild], kept_for[kBuildKey];
    size_t length = 0, marker_length = 0;
    memset(result, 0, sizeof(*result));
    int error = ops->read(ops->context, switch_icon_cache_request, build, sizeof(build), &length);
    /* A stopped drop is redone even for the marker's own build: the cache may hold another build's folders, and the
     * marker's are in the keep. */
    const bool stopped = !error && length && switch_icon_cache_stopped(ops);
    if (error != ENOENT && (error || length) &&
        (error || stopped || !ops->holds(ops->context, switch_icon_cache_marker, build, length))) {
        result->requested = true;
        if (!error) {
            switch_icon_cache_key(build, length, result->build);
        }
        const bool known = SWITCH_ICON_CACHE_SALTED && !error && !stopped &&
                           !ops->read(ops->context, switch_icon_cache_marker, marker, sizeof(marker), &marker_length) &&
                           switch_icon_cache_key(marker, marker_length, kept_for);
#if SWITCH_ICON_CACHE_SALTED
        if (!error) {
            error = ops->write(ops->context, switch_icon_cache_attempt, build, length);
        }
#endif
        if (!error) {
            error =
                switch_icon_cache_clear(ops, result, known ? kept_for : NULL, result->build[0] ? result->build : NULL);
        }
#if DEVELOPMENT || DEBUG
        if (!error && result->restored) {
            error = ops->write(ops->context, switch_icon_cache_restored, build, length);
        }
#endif
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
