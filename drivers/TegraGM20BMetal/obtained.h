// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace gm20b {

// How a state's creation got its record (debug builds: the HW Test reads it, TegraGM20BMetalCompileProbe.mm)
struct Obtained {
    const char *how = "none";  // "pack", "table", "store", "compiled", "joined", "stored"
    uint64_t waitUs = 0;       // the creating thread's wait for the shader compiler
    uint32_t qos = 0;          // that thread's class
    uint32_t ran = 0;          // the compiler's answer: how, and the class it ran at (TegraGM20BCompilerABI.h)
    // waitUs split at a debug compiler's compile start and end (TegraGM20BAbiCompileTimes), its answer, and this
    // process's having the answer, then finding the record; -1 for a part neither side measured. The queue is negative
    // for a request that joined a compile already running.
    int64_t queueUs = -1, compileUs = -1, answerUs = -1, wakeUs = -1, storeUs = -1;
    uint32_t answerPriority = 0;  // the compiler's answering thread's scheduler priority

    // Times in CLOCK_UPTIME_RAW: sent and found in microseconds, the rest in nanoseconds, 0 where not measured
    void split(uint64_t sentUs, uint64_t compileStartNs, uint64_t compileEndNs, uint64_t answeredNs, uint64_t hadNs,
               uint64_t foundUs)
    {
        const auto us = [](uint64_t from, uint64_t to) {
            return (int64_t(to) - int64_t(from)) / 1000;
        };
        if (compileStartNs && compileEndNs) {
            queueUs = us(sentUs * 1000, compileStartNs);
            compileUs = us(compileStartNs, compileEndNs);
        }
        if (compileEndNs && answeredNs)
            answerUs = us(compileEndNs, answeredNs);
        if (answeredNs && hadNs)
            wakeUs = us(answeredNs, hadNs);
        if (hadNs)
            storeUs = us(hadNs, foundUs * 1000);
    }
};

}  // namespace gm20b
