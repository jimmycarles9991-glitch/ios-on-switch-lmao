// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <unistd.h>

struct switch_bootstrap_stderr {
    char text[769];
    size_t used;
};

static inline bool switch_bootstrap_drain_stderr(int descriptor, struct switch_bootstrap_stderr *output)
{
    char bytes[4096];
    const size_t limit = sizeof(output->text) - 1;
    for (unsigned read_count = 0; read_count < 16; ++read_count) {
        const ssize_t result = read(descriptor, bytes, sizeof(bytes));
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return result == 0;
        const size_t count = (size_t)result;
        const size_t keep = count < limit ? count : limit;
        const size_t previous = output->used < limit - keep ? output->used : limit - keep;
        memmove(output->text, output->text + output->used - previous, previous);
        for (size_t index = 0; index < keep; ++index) {
            const unsigned char byte = bytes[count - keep + index];
            output->text[previous + index] = byte >= ' ' && byte <= '~' ? (char)byte : ' ';
        }
        output->used = previous + keep;
        output->text[output->used] = '\0';
    }
    return false;
}

static inline int switch_bootstrap_wait_child(pid_t child, unsigned timeout, int stderr_pipe,
                                              struct switch_bootstrap_stderr *output, int *status, const char **stage)
{
    int error = 0;
    const int queue = kqueue();
    if (queue < 0) {
        *stage = "kqueue";
        error = errno;
        kill(-child, SIGKILL);
    } else {
        struct kevent events[3];
        EV_SET(&events[0], (uintptr_t)child, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
        EV_SET(&events[1], 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_SECONDS, timeout, NULL);
        if (stderr_pipe >= 0)
            EV_SET(&events[2], (uintptr_t)stderr_pipe, EVFILT_READ, EV_ADD, 0, 0, NULL);
        if (kevent(queue, events, stderr_pipe >= 0 ? 3 : 2, NULL, 0, NULL) < 0) {
            if (errno != ESRCH) {
                *stage = "register";
                error = errno;
            }
        } else {
            bool exited = false;
            while (!error && !exited) {
                const int count = kevent(queue, NULL, 0, events, 3, NULL);
                if (count < 0 && errno != EINTR) {
                    *stage = "event-wait";
                    error = errno;
                }
                for (int index = 0; index < count && !error && !exited; ++index) {
                    const struct kevent *event = &events[index];
                    if (event->flags & EV_ERROR) {
                        *stage = "event";
                        error = event->data ? (int)event->data : EIO;
                    } else if (event->filter == EVFILT_TIMER) {
                        *stage = "timeout";
                        error = ETIMEDOUT;
                    } else if (event->filter == EVFILT_PROC) {
                        exited = true;
                    } else if (event->filter == EVFILT_READ && switch_bootstrap_drain_stderr(stderr_pipe, output)) {
                        struct kevent remove;
                        EV_SET(&remove, (uintptr_t)stderr_pipe, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                        int deleted;
                        do {
                            deleted = kevent(queue, &remove, 1, NULL, 0, NULL);
                        } while (deleted < 0 && errno == EINTR);
                        if (deleted < 0 && errno != ENOENT) {
                            *stage = "stderr-event";
                            error = errno;
                        }
                    }
                }
            }
        }
        close(queue);
        if (error)
            kill(-child, SIGKILL);
    }
    while (waitpid(child, status, 0) < 0) {
        if (errno != EINTR) {
            *stage = "wait";
            return errno;
        }
    }
    if (stderr_pipe >= 0)
        switch_bootstrap_drain_stderr(stderr_pipe, output);
    if (error)
        return error;
    *stage = WIFEXITED(*status) ? "child-exit" : "child-signal";
    return WIFEXITED(*status) && !WEXITSTATUS(*status) ? 0 : EIO;
}
