// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <os/log.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sysexits.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

static bool switch_fast_timeout;
static bool switch_error_event;
static unsigned switch_expected_timeout = 60;
static bool switch_expected_stderr;
static bool switch_fake_process;
static std::string switch_failed_stage;
static std::vector<std::string> switch_process_logs;
static int switch_fake_exit;
static unsigned switch_temporary_exits;
static unsigned switch_spawn_count;
static unsigned switch_delay_seconds;

static void switch_test_log(const char *format, ...)
{
    std::string plain = format;
    for (size_t index; (index = plain.find("{public}")) != std::string::npos;)
        plain.erase(index, strlen("{public}"));
    va_list arguments;
    va_start(arguments, format);
    char line[1024];
    vsnprintf(line, sizeof(line), plain.c_str(), arguments);
    va_end(arguments);
    switch_process_logs.emplace_back(line);
}

static int switch_test_spawn(pid_t *child, const char *path, const posix_spawn_file_actions_t *actions,
                             const posix_spawnattr_t *attributes, char *const arguments[], char *const environment[])
{
    if (!switch_fake_process)
        return posix_spawn(child, path, actions, attributes, arguments, environment);
    ++switch_spawn_count;
    if (arguments[1] && arguments[2] && !strcmp(arguments[2], "--switch-bootstrap-result")) {
        assert(!strcmp(path, "/var/jb/usr/bin/launchctl"));
        assert(!strcmp(arguments[1], "bootstrap") || !strcmp(arguments[1], "bootout"));
        assert(!strcmp(arguments[3], "system"));
        assert(!strcmp(arguments[4], "/var/jb/Library/LaunchDaemons/test.plist"));
    }
    short flags;
    pid_t group;
    assert(!posix_spawnattr_getflags(attributes, &flags) && flags == POSIX_SPAWN_SETPGROUP);
    assert(!posix_spawnattr_getpgroup(attributes, &group) && group == 0);
    *child = 42;
    return switch_failed_stage == "spawn" ? EPERM : 0;
}

static int switch_test_kqueue()
{
    if (switch_fake_process && switch_failed_stage == "kqueue") {
        errno = EPERM;
        return -1;
    }
    return kqueue();
}

static int switch_test_kill(pid_t child, int signal)
{
    if (!switch_fake_process)
        return kill(child, signal);
    assert(child == -42 && signal == SIGKILL);
    return 0;
}

static pid_t switch_test_wait(pid_t child, int *status, int options)
{
    if (!switch_fake_process)
        return waitpid(child, status, options);
    assert(child == 42 && options == 0);
    if (switch_failed_stage == "wait") {
        errno = ECHILD;
        return -1;
    }
    *status = switch_failed_stage == "child-exit"     ? 77 << 8
              : switch_failed_stage == "child-signal" ? SIGKILL
                                                      : switch_fake_exit << 8;
    if (switch_temporary_exits) {
        --switch_temporary_exits;
        *status = EX_TEMPFAIL << 8;
    }
    return child;
}

static unsigned switch_test_sleep(unsigned seconds)
{
    switch_delay_seconds += seconds;
    return 0;
}

static int switch_test_kevent(int queue, const struct kevent *changes, int count, struct kevent *events, int capacity,
                              const struct timespec *timeout)
{
    if (switch_fake_process) {
        if (changes) {
            assert(count == (switch_expected_stderr ? 3 : 2));
            assert(changes[0].filter == EVFILT_PROC && changes[0].ident == 42);
            assert(changes[1].filter == EVFILT_TIMER && changes[1].data == switch_expected_timeout);
            if (count == 3)
                assert(changes[2].filter == EVFILT_READ);
            if (switch_failed_stage == "register") {
                errno = EPERM;
                return -1;
            }
            return 0;
        }
        assert(events && capacity == 3);
        if (switch_failed_stage == "event-wait") {
            errno = EIO;
            return -1;
        }
        if (switch_failed_stage == "event")
            EV_SET(events, 42, EVFILT_PROC, EV_ERROR, 0, EPERM, nullptr);
        else if (switch_failed_stage == "timeout")
            EV_SET(events, 1, EVFILT_TIMER, 0, 0, 0, nullptr);
        else
            EV_SET(events, 42, EVFILT_PROC, 0, NOTE_EXIT, 0, nullptr);
        return 1;
    }
    if (switch_error_event && events && capacity) {
        EV_SET(events, 1, EVFILT_PROC, EV_ERROR, 0, EPERM, nullptr);
        switch_error_event = false;
        return 1;
    }
    struct kevent shortened[3];
    if (switch_fast_timeout && changes) {
        assert(count <= 3);
        for (int index = 0; index < count; ++index) {
            shortened[index] = changes[index];
            if (shortened[index].filter == EVFILT_TIMER)
                shortened[index].data = 1;
        }
        changes = shortened;
    }
    return kevent(queue, changes, count, events, capacity, timeout);
}

#define kevent(...) switch_test_kevent(__VA_ARGS__)
#define kqueue      switch_test_kqueue
#define posix_spawn switch_test_spawn
#define kill        switch_test_kill
#define waitpid     switch_test_wait
#define sleep       switch_test_sleep
#undef os_log_error
#define os_log_error(log, ...) switch_test_log(__VA_ARGS__)
extern "C" {
#include "drivers/SwitchBootstrap/process.c"
}
#undef kevent
#undef kqueue
#undef posix_spawn
#undef kill
#undef waitpid
#undef sleep
#undef os_log_error

int main()
{
    char success[] = "/usr/bin/true";
    char *const successful[] = {success, nullptr};
    assert(switch_bootstrap_run(success, successful) == 0);
    char failure[] = "/usr/bin/false";
    char *const failed[] = {failure, nullptr};
    assert(switch_bootstrap_run(failure, failed) == EIO);
    assert(switch_process_logs.back() == "SwitchBootstrap: /usr/bin/false exited with status 1");
    char absent[] = "/does-not-exist/switch-test";
    char *const missing[] = {absent, nullptr};
    assert(switch_bootstrap_run(absent, missing) == ENOENT);
    char shell[] = "/bin/sh";
    char option[] = "-c";
    char signal[] = "kill -TERM $$";
    char *const signaled[] = {shell, option, signal, nullptr};
    assert(switch_bootstrap_run(shell, signaled) == EIO);
    assert(switch_process_logs.back() == "SwitchBootstrap: /bin/sh killed by signal 15");
    char noisy[] = "i=0; while [ $i -lt 4096 ]; do printf 'stderr output line\\n' >&2; i=$((i+1)); done; "
                   "printf '\\033[0mfinal diagnostic\\n' >&2; exit 17";
    char *const loud_failure[] = {shell, option, noisy, nullptr};
    switch_process_logs.clear();
    assert(switch_bootstrap_run_update(shell, loud_failure) == EIO);
    assert(switch_process_logs.size() == 2);
    assert(switch_process_logs[0] == "SwitchBootstrap: /bin/sh exited with status 17");
    const std::string stderr_prefix = "SwitchBootstrap: /bin/sh stderr (tail): ";
    assert(switch_process_logs[1].find(stderr_prefix) == 0);
    assert(switch_process_logs[1].size() == stderr_prefix.size() + 768);
    assert(switch_process_logs[1].find(" [0mfinal diagnostic ") != std::string::npos);
    int pipe_ends[2];
    assert(!pipe(pipe_ends));
    assert(!fcntl(pipe_ends[0], F_SETFL, O_NONBLOCK));
    assert(write(pipe_ends[1], "tail", 4) == 4);
    struct switch_bootstrap_stderr output = {{0}, 0};
    assert(!switch_bootstrap_drain_stderr(pipe_ends[0], &output));
    assert(!strcmp(output.text, "tail"));
    close(pipe_ends[1]);
    assert(switch_bootstrap_drain_stderr(pipe_ends[0], &output));
    close(pipe_ends[0]);
    char update[] = "test \"$SWITCH_BOOTSTRAP_UPDATE\" = 1";
    char *const updating[] = {shell, option, update, nullptr};
    assert(switch_bootstrap_run_update(shell, updating) == 0);
    assert(switch_bootstrap_run(shell, updating) == EIO);
    char prep[] =
        "test \"$PATH\" = '/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin' "
        "&& test \"$SWITCH_BOOTSTRAP_UPDATE\" = 0 || exit 42; printf 'prep diagnostic\\n' >&2; exit 127";
    char *const preparing[] = {shell, option, prep, nullptr};
    switch_process_logs.clear();
    assert(switch_bootstrap_run_prep(shell, preparing) == EIO);
    assert(switch_process_logs.size() == 2);
    assert(switch_process_logs[0] == "SwitchBootstrap: /bin/sh exited with status 127");
    assert(switch_process_logs[1] == stderr_prefix + "prep diagnostic ");
    char sleeper[] = "sleep 30";
    char *const sleeping[] = {shell, option, sleeper, nullptr};
    switch_error_event = true;
    assert(switch_bootstrap_run(shell, sleeping) == EPERM);
    switch_fast_timeout = true;
    assert(switch_bootstrap_run(shell, sleeping) == ETIMEDOUT);

    switch_fake_process = true;
    for (const auto &[stage, error] : std::vector<std::pair<std::string, int>>{{"spawn", EPERM},
                                                                               {"kqueue", EPERM},
                                                                               {"register", EPERM},
                                                                               {"event-wait", EIO},
                                                                               {"event", EPERM},
                                                                               {"timeout", ETIMEDOUT},
                                                                               {"wait", ECHILD},
                                                                               {"child-exit", EIO},
                                                                               {"child-signal", EIO}}) {
        switch_failed_stage = stage;
        assert(switch_bootstrap_run(success, successful) == error);
    }
    switch_failed_stage.clear();
    assert(switch_bootstrap_run(success, successful) == 0);
    switch_expected_stderr = true;
    assert(switch_bootstrap_run_prep(shell, preparing) == 0);
    int daemon_status;
    const char *plist = "/var/jb/Library/LaunchDaemons/test.plist";
    for (int code :
         {SWITCH_LAUNCHCTL_CHANGED, SWITCH_LAUNCHCTL_PRESENT, SWITCH_LAUNCHCTL_DISABLED, SWITCH_LAUNCHCTL_ABSENT}) {
        switch_fake_exit = code;
        assert(!switch_bootstrap_run_daemon("bootstrap", plist, &daemon_status) && daemon_status == code);
    }
    switch_fake_exit = SWITCH_LAUNCHCTL_FAILED;
    assert(switch_bootstrap_run_daemon("bootstrap", plist, &daemon_status) == EIO);
    switch_fake_exit = SWITCH_LAUNCHCTL_DISABLED;
    assert(switch_bootstrap_run_update(shell, preparing) == EIO);
    switch_failed_stage = "timeout";
    assert(switch_bootstrap_run_daemon("bootstrap", plist, &daemon_status) == ETIMEDOUT);
    assert(switch_bootstrap_run_daemon("exec", plist, &daemon_status) == EINVAL);
    switch_failed_stage.clear();
    switch_fake_exit = 0;
    switch_expected_timeout = 180;
    char registrar[] = "/usr/libexec/switch-appregd";
    char register_sileo[] = "--register-sileo";
    char *const registration[] = {registrar, register_sileo, nullptr};
    assert(switch_bootstrap_run(registrar, registration) == 0);
    char finish[] = "--finish-migration";
    char *const migration[] = {registrar, finish, nullptr};
    switch_expected_timeout = 185;
    switch_temporary_exits = 1;
    switch_spawn_count = 0;
    assert(switch_bootstrap_run(registrar, migration) == 0);
    assert(switch_spawn_count == 2 && switch_delay_seconds == 1);
    switch_fake_exit = EX_UNAVAILABLE;
    switch_spawn_count = 0;
    assert(switch_bootstrap_run(registrar, migration) == EIO && switch_spawn_count == 1);
    switch_fake_exit = EX_TEMPFAIL;
    switch_spawn_count = 0;
    switch_expected_timeout = 180;
    assert(switch_bootstrap_run(registrar, registration) == EIO && switch_spawn_count == 1);
    switch_expected_timeout = 185;
    switch_spawn_count = switch_delay_seconds = 0;
    assert(switch_bootstrap_run(registrar, migration) == EIO && switch_spawn_count == 3 && switch_delay_seconds == 3);
    switch_bootstrap_set_budget(1500);
    switch_expected_timeout = 1;
    switch_spawn_count = switch_delay_seconds = 0;
    assert(switch_bootstrap_run(registrar, migration) == ETIMEDOUT && switch_spawn_count == 1 && !switch_delay_seconds);
}
