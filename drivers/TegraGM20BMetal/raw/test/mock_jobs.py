#!/usr/bin/env python3
"""mock_jobs: raw/pack/mock_iokit.c as the harnesses here build it, with the job methods (TegraGM20BJobABI.h).

The raw runtime submits only jobs (SubmitJob on a resource set); the pack's mock knows Exec alone, and raw/pack/** is
hashed by the raw stack key, so the harnesses add the methods to their own copy: a set is made whole, its generation's
mirror at TegraGM20BSetGenerationWord, and a job runs as the Exec of its pushes (MOCK_JOB_LOG=1: each job's pushes and
uses on stderr).
usage: mock_jobs.py <copy of mock_iokit.c>
"""

import sys

OLD = """    case TegraGM20BMethodSubmitAddress:
        return mock_submit(c, in, insz, out, outsz);"""

NEW = """    case TegraGM20BMethodSubmitAddress:
        return mock_submit(c, in, insz, out, outsz);
    case TegraGM20BMethodResourceSetCreate: {
        if (!fence)
            fence = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
        fence[TegraGM20BSetGenerationWord] = 1;
        struct TegraGM20BAbiResourceSetCreateReply r = {.set = 1, .generation = 1, .fence_token = kFenceToken,
                                                        .generation_offset = TegraGM20BSetGenerationWord * 4};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodResourceSetRelease:
        return KERN_SUCCESS;
    case TegraGM20BMethodSubmitJob: {
        const struct TegraGM20BAbiSubmitAddress *address = in;
        const struct TegraGM20BAbiJob *job = (const void *)(uintptr_t)address->address;
        const size_t bytes = 8 + job->push_count * sizeof(struct TegraGM20BAbiPush);
        uint32_t *exec = calloc(1, bytes < sizeof(struct TegraGM20BAbiExec) ? sizeof(struct TegraGM20BAbiExec) : bytes);
        exec[0] = job->channel;
        exec[1] = job->push_count;
        memcpy(exec + 2, job + 1, job->push_count * sizeof(struct TegraGM20BAbiPush));
        const kern_return_t kr = mock_call(c, TegraGM20BMethodExec, exec, bytes, out, outsz);
        if (getenv("MOCK_JOB_LOG")) {
            fprintf(stderr, "job pushes %u set %u uses %u\\n", job->push_count, job->set, job->shared_count);
            const struct TegraGM20BAbiSurfaceUse *u =
                (const void *)((const struct TegraGM20BAbiPush *)(job + 1) + job->push_count);
            for (uint32_t i = 0; i < job->shared_count; i++)
                fprintf(stderr, "use %llx %x\\n", (unsigned long long)u[i].handle, u[i].access);
        }
        free(exec);
        return kr;
    }"""


def main() -> None:
    path = sys.argv[1]
    with open(path) as f:
        source = f.read()
    if 'TegraGM20BMethodSubmitJob' in source:
        return
    source = source.replace(
        '#include "TegraGM20BABI.h"', '#include "TegraGM20BABI.h"\n#include "TegraGM20BJobABI.h"', 1
    )
    if source.count(OLD) != 1:
        sys.exit('mock_jobs: mock_iokit.c no longer has the SubmitAddress case this patch follows')
    with open(path, 'w') as f:
        f.write(source.replace(OLD, NEW))


if __name__ == '__main__':
    main()
