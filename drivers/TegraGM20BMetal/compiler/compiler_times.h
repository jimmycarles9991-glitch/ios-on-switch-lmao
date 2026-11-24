// SPDX-License-Identifier: MIT
// Debug compilers: where an answered request's wait went (TegraGM20BCompilerABI.h TegraGM20BAbiCompileTimes); release
// compilers send zeros. A job's compile is timed on the worker that runs it, and the pool answers that job's waiters on
// the same worker (compiler_pool.h), so the times reach the answers through that thread's own copy. An answer from the
// thread that takes requests (a stored record, a request joining a job already answered) carries no compile.
#pragma once
#include <mach/mach.h>
#include <pthread.h>
#include <time.h>
#include "TegraGM20BCompilerABI.h"

namespace switch_compiler {

#if SWITCH_DEBUG
inline thread_local TegraGM20BAbiCompileTimes tJobTimes;

// Where a job's compile went, for its line (compiler_service.mm): the worker's own, cleared as the job starts. Stages
// are NIR and NAK's compiles (compiler_stages.h); translationsKept is how many more of m2v's the process keeps after
// the job than before it (nmb_kept_translations), which another worker's job may move too.
struct JobSplit {
    uint64_t libraryNs = 0, makeNs = 0, codegenNs = 0, storeNs = 0;
    uint64_t stagesAsked = 0, stagesMade = 0;
    int64_t translationsKept = 0;
};

inline thread_local JobSplit tJobSplit;
#define SWITCH_JOB_COUNT(field) (::switch_compiler::tJobSplit.field++)

inline uint64_t uptimeNs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

inline void compileStarted()
{
    tJobTimes = {uptimeNs(), 0, 0, 0, 0};
}

inline void compileEnded()
{
    tJobTimes.compile_end = uptimeNs();
}

inline void noCompile()
{
    tJobTimes = {};
}

// The scheduler priority is the one in force, so a class the task's policy caps shows here and not in qos_class_self()
inline TegraGM20BAbiCompileTimes answering()
{
    TegraGM20BAbiCompileTimes times = tJobTimes;
    thread_extended_info_data_t info = {};
    mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
    if (thread_info(pthread_mach_thread_np(pthread_self()), THREAD_EXTENDED_INFO, thread_info_t(&info), &count) ==
        KERN_SUCCESS)
        times.answer_priority = uint32_t(info.pth_curpri);
    times.answered = uptimeNs();
    return times;
}
#else
#define SWITCH_JOB_COUNT(field) ((void)0)

inline void compileStarted()
{}

inline void compileEnded()
{}

inline void noCompile()
{}

inline TegraGM20BAbiCompileTimes answering()
{
    return {};
}
#endif

}  // namespace switch_compiler
