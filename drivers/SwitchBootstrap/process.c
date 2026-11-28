// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "process.h"
#include "process_wait.h"
#include "bootstrap/launchctl/daemon_result.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <os/log.h>
#include <stdbool.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sysexits.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t switch_bootstrap_deadline;

void switch_bootstrap_set_budget(unsigned milliseconds)
{
    switch_bootstrap_deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + (uint64_t)milliseconds * 1000000;
}

unsigned switch_bootstrap_remaining_ms(void)
{
    if (!switch_bootstrap_deadline)
        return UINT_MAX;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    return now < switch_bootstrap_deadline ? (unsigned)((switch_bootstrap_deadline - now) / 1000000) : 0;
}

static int switch_bootstrap_stderr_pipe(int descriptors[2], posix_spawn_file_actions_t *actions)
{
    if (pipe(descriptors))
        return errno;
    for (unsigned index = 0; index < 2; ++index) {
        if (descriptors[index] <= STDERR_FILENO) {
            const int descriptor = fcntl(descriptors[index], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (descriptor < 0)
                return errno;
            close(descriptors[index]);
            descriptors[index] = descriptor;
        } else if (fcntl(descriptors[index], F_SETFD, FD_CLOEXEC) < 0) {
            return errno;
        }
    }
    if (fcntl(descriptors[0], F_SETFL, O_NONBLOCK) < 0)
        return errno;
    int error = posix_spawn_file_actions_adddup2(actions, descriptors[1], STDERR_FILENO);
    if (!error)
        error = posix_spawn_file_actions_addclose(actions, descriptors[0]);
    if (!error)
        error = posix_spawn_file_actions_addclose(actions, descriptors[1]);
    return error;
}

static int switch_bootstrap_run_command(const char *path, char *const arguments[], bool update, bool capture_stderr,
                                        unsigned timeout, int *daemon_status, bool *temporary_exit)
{
    if (switch_bootstrap_remaining_ms() < 1000)
        return ETIMEDOUT;
    posix_spawnattr_t attributes;
    int error = posix_spawnattr_init(&attributes);
    if (error)
        return error;
    if (!(error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP))) {
        error = posix_spawnattr_setpgroup(&attributes, 0);
    }
    const bool registration = !strcmp(path, "/usr/libexec/switch-appregd");
    char update_environment[] = "SWITCH_BOOTSTRAP_UPDATE=0";
    update_environment[sizeof(update_environment) - 2] = update ? '1' : '0';
    char *const environment[] = {
        "PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin",
        "HOME=/var/root",
        "NO_PASSWORD_PROMPT=1",
        "_MSSafeMode=1",
        "_SafeMode=1",
        update_environment,
        NULL,
    };
    posix_spawn_file_actions_t actions;
    int descriptors[2] = {-1, -1};
    bool have_actions = false;
    if (!error && (capture_stderr || registration || !strcmp(path, "/var/jb/usr/bin/launchctl"))) {
        error = posix_spawn_file_actions_init(&actions);
        have_actions = !error;
    }
    if (have_actions)
        error = switch_bootstrap_stderr_pipe(descriptors, &actions);
    pid_t child = -1;
    if (!error) {
#if SWITCH_DEBUG
        os_log(OS_LOG_DEFAULT, "SwitchBootstrap: launching %{public}s", path);
#endif
        error = posix_spawn(&child, path, have_actions ? &actions : NULL, &attributes, arguments, environment);
    }
    if (have_actions)
        posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (descriptors[1] >= 0)
        close(descriptors[1]);
    if (error) {
        if (descriptors[0] >= 0)
            close(descriptors[0]);
        os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: spawn %{public}s failed (%d)", path, error);
        return error;
    }
    // The registration client's XPC reply timeout is 180 seconds. Let it report EX_TEMPFAIL before killing it.
    if (temporary_exit)
        timeout += 5;
    const unsigned remaining = switch_bootstrap_remaining_ms() / 1000;
    if (timeout > remaining)
        timeout = remaining ? remaining : 1;
    const char *stage = NULL;
    struct switch_bootstrap_stderr output = {{0}, 0};
    int status = 0;
    error = switch_bootstrap_wait_child(child, timeout, descriptors[0], &output, &status, &stage);
    if (descriptors[0] >= 0)
        close(descriptors[0]);
    if (temporary_exit && stage && !strcmp(stage, "child-exit"))
        *temporary_exit = WEXITSTATUS(status) == EX_TEMPFAIL;
    if (daemon_status && stage && !strcmp(stage, "child-exit")) {
        const int code = WEXITSTATUS(status);
        if (code == SWITCH_LAUNCHCTL_CHANGED || code == SWITCH_LAUNCHCTL_PRESENT || code == SWITCH_LAUNCHCTL_DISABLED ||
            code == SWITCH_LAUNCHCTL_ABSENT) {
            *daemon_status = code;
            return 0;
        }
    }
    if (error) {
        if (!strcmp(stage, "child-exit"))
            os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: %{public}s exited with status %d", path,
                         WEXITSTATUS(status));
        else if (!strcmp(stage, "child-signal"))
            os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: %{public}s killed by signal %d", path, WTERMSIG(status));
        else
            os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: %{public}s %{public}s failed (%d)", path, stage, error);
        if (output.used)
            os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: %{public}s stderr (tail): %{public}s", path, output.text);
    }
    return error;
}

int switch_bootstrap_run(const char *path, char *const arguments[])
{
    const bool registration = !strcmp(path, "/usr/libexec/switch-appregd");
    const bool finish_migration = registration && arguments[1] && !strcmp(arguments[1], "--finish-migration") &&
                                  !arguments[2];
    for (unsigned attempt = 0;; ++attempt) {
        bool temporary_exit = false;
        const int error = switch_bootstrap_run_command(path, arguments, false, false, registration ? 180 : 60, NULL,
                                                       finish_migration ? &temporary_exit : NULL);
        if (!temporary_exit || attempt == 2)
            return error;
        const unsigned backoff = attempt + 1;
        if (switch_bootstrap_remaining_ms() < (backoff + 1) * 1000)
            return ETIMEDOUT;
        sleep(backoff);
    }
}

int switch_bootstrap_run_prep(const char *path, char *const arguments[])
{
    return switch_bootstrap_run_command(path, arguments, false, true, 240, NULL, NULL);
}

int switch_bootstrap_run_update(const char *path, char *const arguments[])
{
    return switch_bootstrap_run_command(path, arguments, true, true, 240, NULL, NULL);
}

int switch_bootstrap_run_daemon(const char *action, const char *plist, int *result)
{
    if (!result || !plist || plist[0] != '/' || (strcmp(action, "bootstrap") && strcmp(action, "bootout")))
        return EINVAL;
    *result = SWITCH_LAUNCHCTL_FAILED;
    char *const arguments[] = {
        (char *)"/var/jb/usr/bin/launchctl",
        (char *)action,
        (char *)"--switch-bootstrap-result",
        (char *)"system",
        (char *)plist,
        NULL,
    };
    return switch_bootstrap_run_command(arguments[0], arguments, false, true, 60, result, NULL);
}
