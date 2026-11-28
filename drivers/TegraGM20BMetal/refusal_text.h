// SPDX-License-Identifier: MIT
// Why a command buffer was not submitted, as CommandBuffer::describeRefusal writes it for the "a command buffer not
// submitted" error line and for the error its Metal command buffer fails with, and which of those lines are logged.
// Plain data, so a host test drives each refusal.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace gm20b {

struct NotSubmitted {
    const char *setRefused = nullptr;  // a draw's or a state's own refusal, named where it was refused
    unsigned kernelResult = 0;         // the kernel's refusal of the job
    unsigned nativeError = 0;          // a view of a surface's own planes the kernel did not make
    const char *notSubmitted = nullptr;
};

// The first reason that holds, in that order: what refused it first is what a reader can fix.
inline void describeRefusal(const NotSubmitted &n, char *line, size_t size)
{
    if (n.setRefused && *n.setRefused)
        snprintf(line, size, "%s", n.setRefused);
    else if (n.kernelResult)
        snprintf(line, size, "the kernel refused its job (0x%x)", n.kernelResult);
    else if (n.nativeError)
        snprintf(line, size, "a view of a surface's own planes was not made (0x%x)", n.nativeError);
    else
        snprintf(line, size, "%s", n.notSubmitted ? n.notSubmitted : "nothing named why");
}

// A refusal its caller takes up again, which gets no error line and is not counted: the CPU holding a use
// (`cannotLock`), or a native surface changed under it (the kernel's result is the view's own error).
inline bool takenUpAgain(const NotSubmitted &r, unsigned cannotLock)
{
    return r.kernelResult == cannotLock || (r.nativeError && r.kernelResult == r.nativeError);
}

// Whether the n-th counted refusal (from 1) gets its error line: the first 16, then every 256th.
inline bool notSubmittedLineDue(uint32_t n)
{
    return n <= 16 || n % 256 == 0;
}

}  // namespace gm20b
