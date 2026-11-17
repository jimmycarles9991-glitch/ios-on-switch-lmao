"""Writes gpu_exec.c with its ownership boundary moved, for the mutant runs.

    switch_gpu_exec_mutant.py <mutant> <gpu_exec.c> <out.c>

Each edit must match exactly once, so a change to the code around the commit fails the mutant's build instead of
quietly testing something else.
"""

import sys

PUBLISH = '\tif (owned)\n\t\tswitch_gpu_backing_publish(owned);\n'
FINISH = '\tif (owned)\n\t\tswitch_gpu_backing_finish(owned);\n'
VALIDATE = '\tret = backing_waits(chan, dependencies, dependency_count, unsettled, 0);\n'
USER_ENTRIES = '\tlast = switch_gpu_job_emit(pushes, count, chan->submitted, &emitter);\n'
DOORBELL = '\tchan_doorbell(chan);\n'
EMPTY_COMMIT = '\t\tif (owned)\n\t\t\tswitch_gpu_backing_commit(owned);\n\t\t*seqno = chan->submitted;\n'

MUTANTS = {
    'after_first_kick': [
        (PUBLISH, ''),
        (USER_ENTRIES, USER_ENTRIES + PUBLISH),
    ],
    'after_doorbell': [
        (PUBLISH, ''),
        (DOORBELL, DOORBELL + PUBLISH),
    ],
    'before_checks': [
        (PUBLISH, ''),
        (VALIDATE, PUBLISH + VALIDATE),
    ],
    'before_finish': [
        (DOORBELL + FINISH, FINISH + DOORBELL),
    ],
    'no_empty_commit': [
        (EMPTY_COMMIT, '\t\t*seqno = chan->submitted;\n'),
    ],
}


def mutate(name: str, text: str) -> str:
    for old, new in MUTANTS[name]:
        count = text.count(old)
        if count != 1:
            sys.exit(f'{name}: expected one match of {old!r}, found {count}')
        text = text.replace(old, new)
    return text


def main() -> None:
    name, source, out = sys.argv[1:]
    with open(source, encoding='utf-8') as f:
        text = f.read()
    with open(out, 'w', encoding='utf-8') as f:
        f.write(mutate(name, text))


if __name__ == '__main__':
    main()
