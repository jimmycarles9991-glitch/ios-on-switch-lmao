// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>

#include "respring.h"

#include <dlfcn.h>
#include <limits.h>
#include <objc/runtime.h>
#include <spawn.h>
#include <stdio.h>
#include <sysexits.h>
#include <time.h>
#include <vector>

#include "../../drivers/SwitchBootstrap/process_wait.h"

extern "C" int proc_listallpids(void *, int);
extern "C" int proc_pidpath(int, void *, uint32_t);

@interface SBSRelaunchAction : NSObject
+ (instancetype)actionWithReason:(NSString *)reason options:(NSUInteger)options targetURL:(NSURL *)url;
@end

@interface FBSSystemService : NSObject
+ (instancetype)sharedService;
- (void)sendActions:(NSSet *)actions withResult:(id)result;
@end

static int switch_respring_snapshot(SwitchRespringServices &services)
{
    const int count = proc_listallpids(nullptr, 0);
    if (count <= 0 || count > INT_MAX / static_cast<int>(sizeof(pid_t)) - 32)
        return EIO;
    std::vector<pid_t> pids(count + 32);
    const int found = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (found <= 0 || static_cast<size_t>(found) >= pids.size())
        return EIO;
    for (int index = 0; index < found; ++index) {
        char path[PATH_MAX];
        if (!pids[index] || proc_pidpath(pids[index], path, sizeof(path)) <= 0)
            continue;
        for (auto &service : services) {
            if (!strcmp(path, service.executable))
                service.pid = pids[index];
        }
    }
    return 0;
}

static int switch_respring_watch(int queue, SwitchRespringServices &services)
{
    for (auto &service : services) {
        if (!service.pid)
            continue;
        struct kevent event;
        EV_SET(&event, static_cast<uintptr_t>(service.pid), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
        if (kevent(queue, &event, 1, nullptr, 0, nullptr) < 0) {
            if (errno != ESRCH)
                return errno;
            service.pid = 0;
        }
    }
    return 0;
}

static int switch_respring_wait(int queue, SwitchRespringServices &services, unsigned seconds)
{
    constexpr uint64_t nsPerSecond = 1000000000;
    const uint64_t deadline = clock_gettime_nsec_np(CLOCK_MONOTONIC) + seconds * nsPerSecond;
    while (services[0].pid || services[1].pid) {
        const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        if (seconds && now >= deadline)
            return ETIMEDOUT;
        const uint64_t remaining = seconds ? deadline - now : 0;
        const timespec timeout = {static_cast<time_t>(remaining / nsPerSecond),
                                  static_cast<long>(remaining % nsPerSecond)};
        struct kevent event;
        const int result = kevent(queue, nullptr, 0, &event, 1, &timeout);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        if (!result)
            return ETIMEDOUT;
        if (event.flags & EV_ERROR)
            return event.data ? static_cast<int>(event.data) : EIO;
        for (auto &service : services) {
            if (event.ident == static_cast<uintptr_t>(service.pid))
                service.pid = 0;
        }
    }
    return 0;
}

static bool switch_respring_frontboard()
{
    if (!dlopen("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices", RTLD_NOW) ||
        !dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_NOW))
        return false;
    Class actionClass = objc_getClass("SBSRelaunchAction");
    Class serviceClass = objc_getClass("FBSSystemService");
    if (![actionClass respondsToSelector:@selector(actionWithReason:options:targetURL:)] ||
        ![serviceClass respondsToSelector:@selector(sharedService)])
        return false;
    constexpr NSUInteger restartRenderServer = 1u << 0;
    constexpr NSUInteger fadeToBlack = 1u << 2;
    SBSRelaunchAction *action = [actionClass actionWithReason:@"respring"
                                                     options:restartRenderServer | fadeToBlack
                                                   targetURL:nil];
    FBSSystemService *service = [serviceClass sharedService];
    if (!action || ![service respondsToSelector:@selector(sendActions:withResult:)])
        return false;
    [service sendActions:[NSSet setWithObject:action] withResult:nil];
    return true;
}

static int switch_respring_kickstart(const char *job)
{
    using SwitchUserSessionsEnabled = int64_t (*)();
    const auto hasUsers = reinterpret_cast<SwitchUserSessionsEnabled>(dlsym(RTLD_DEFAULT, "xpc_user_sessions_enabled"));
    char target[96];
    const int length = snprintf(target, sizeof(target), "%s/%s", hasUsers && hasUsers() ? "user/foreground" : "system",
                                job);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(target))
        return EINVAL;

    posix_spawnattr_t attributes;
    int error = posix_spawnattr_init(&attributes);
    if (error)
        return error;
    pid_t child = 0;
    error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    if (!error)
        error = posix_spawnattr_setpgroup(&attributes, 0);
    const char *const path = "/var/jb/usr/bin/launchctl";
    char *const arguments[] = {const_cast<char *>(path), const_cast<char *>("kickstart"), const_cast<char *>("-k"),
                               target, nullptr};
    char *const environment[] = {const_cast<char *>("PATH=/var/jb/usr/bin:/usr/bin:/bin"), nullptr};
    if (!error)
        error = posix_spawn(&child, path, nullptr, &attributes, arguments, environment);
    posix_spawnattr_destroy(&attributes);
    if (error)
        return error;
    int status = 0;
    const char *stage = nullptr;
    error = switch_bootstrap_wait_child(child, 5, -1, nullptr, &status, &stage);
    if (error)
        fprintf(stderr, "launchctl kickstart %s failed (%s, wait status %#x)\n", target, stage,
                static_cast<unsigned>(status));
    return error;
}

int main(int argc, char **)
{
    if (argc != 1)
        return EX_USAGE;
    @autoreleasepool {
        SwitchRespringServices services = {{
            {"/usr/libexec/backboardd", "com.apple.backboardd"},
            {"/System/Library/CoreServices/SpringBoard.app/SpringBoard", "com.apple.SpringBoard"},
        }};
        const SwitchRespringOperations operations = {
            switch_respring_watch, switch_respring_frontboard, switch_respring_wait, switch_respring_kickstart,
        };
        int error = switch_respring_snapshot(services);
        if (!error) {
            const int queue = kqueue();
            if (queue < 0) {
                error = errno;
            } else {
                error = switch_respring(queue, services, operations);
                close(queue);
            }
        }
        if (error)
            fprintf(stderr, "SpringBoard restart failed (%d): %s\n", error, strerror(error));
        return error ? EX_SOFTWARE : EX_OK;
    }
}
