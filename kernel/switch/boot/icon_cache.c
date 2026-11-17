// SPDX-License-Identifier: GPL-2.0-only
/*
 * <switch/icon_cache.h>. iconservicesagent keeps the icons it rendered while their __cache_info__ names the system's
 * ProductBuildVersion (-[ISMutableIconCache initWithURL:], 21H461 and 24B119), which our GPU build does not change, so
 * icons a GPU build drew outlive it. The deploy names the GPU build it staged in Switch/icon-cache-gpu
 * (tools/deploy/icon_cache.py) and leaves the cache alone, which the Mac's user can be refused. When that name is not
 * the marker's, the build the cache was last dropped for, each __cache_info__ goes and the agent empties the cache at
 * its first launch, as after an OS update; the directories keep their owner and ACL, the agent's other two conditions.
 * Then the marker.
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
    kBuild = 128
};

/* __cache_info__ in the cache folder, or in its salt folder `salt`; none there is no error. */
static int switch_icon_cache_unlink(const struct switch_icon_cache_ops *ops, struct switch_icon_cache_result *result,
                                    const char *salt)
{
    static const char info[] = "/__cache_info__";
    char path[kPath];
    const size_t folder = strlen(switch_icon_cache_folder), named = salt ? 1 + strlen(salt) : 0;
    if (folder + named + sizeof(info) > sizeof(path)) {
        return ENAMETOOLONG;
    }
    memcpy(path, switch_icon_cache_folder, folder);
    if (salt) {
        path[folder] = '/';
        memcpy(path + folder + 1, salt, named - 1);
    }
    memcpy(path + folder + named, info, sizeof(info));
    const int error = ops->unlink(ops->context, path);
    if (!error) {
        ++result->dropped;
    }
    return error == ENOENT ? 0 : error;
}

#if SWITCH_ICON_CACHE_SALTED
struct switch_icon_cache_walk {
    const struct switch_icon_cache_ops *ops;
    struct switch_icon_cache_result *result;
};

static int switch_icon_cache_salt(void *walk, const char *name)
{
    const struct switch_icon_cache_walk *each = walk;
    return switch_icon_cache_unlink(each->ops, each->result, name);
}
#endif

static int switch_icon_cache_unlink_all(const struct switch_icon_cache_ops *ops,
                                        struct switch_icon_cache_result *result)
{
#if SWITCH_ICON_CACHE_SALTED
    struct switch_icon_cache_walk walk = {ops, result};
    const int error = ops->folders(ops->context, switch_icon_cache_folder, switch_icon_cache_salt, &walk);
#else
    const int error = switch_icon_cache_unlink(ops, result, NULL);
#endif
    return error == ENOENT ? 0 : error;
}

void switch_icon_cache_drop(const struct switch_icon_cache_ops *ops, struct switch_icon_cache_result *result)
{
    char build[kBuild];
    size_t length = 0;
    memset(result, 0, sizeof(*result));
    int error = ops->read(ops->context, switch_icon_cache_request, build, sizeof(build), &length);
    if (error == ENOENT || (!error && !length)) {
        return;
    }
    if (!error && ops->holds(ops->context, switch_icon_cache_marker, build, length)) {
        return;
    }
    result->requested = true;
    for (size_t i = 0; !error && i < length && i < sizeof(result->build) - 1 && build[i] != '\n'; ++i) {
        result->build[i] = build[i];
    }
    if (!error) {
        error = switch_icon_cache_unlink_all(ops, result);
    }
    if (!error) {
        error = ops->write(ops->context, switch_icon_cache_marker, build, length);
    }
    result->error = error;
}
