"""Writes gpu_exec.c with its commit point moved, for the commit test's mutant runs.

    switch_gpu_exec_mutant.py <mutant> <gpu_exec.c> <out.c>

Each edit must match exactly once, so a change to the code around the commit fails the mutant's build instead of
quietly testing something else.
"""

import sys

COMMIT = '\tif (owned)\n\t\tswitch_gpu_backing_commit(owned);\n'
PASS_ONE = '\ttgm_backing_dependencies(chan, dependencies, dependency_count, 1);\n'
PASS_ZERO = '\tret = tgm_backing_dependencies(chan, dependencies, dependency_count, 0);\n'
FIRST_KICK = '\tPUSH_KICK(&chan->chan.push);\n\ttgm_chan_park_promises(chan, dependencies, dependency_count);\n'
FENCE_KICK = (
    '\t*seqno = chan_fence_job(chan, external, chan->submitted + 1, last, quiet, releases,\n\t\t\t\trelease_count);\n'
)
EMPTY_COMMIT = '\t\tif (owned)\n\t\t\tswitch_gpu_backing_commit(owned);\n\t\t*seqno = chan->submitted;\n'

MUTANTS = {
    'after_first_kick': [
        (COMMIT + PASS_ONE, PASS_ONE),
        (FIRST_KICK, FIRST_KICK.replace('\ttgm_chan_park_promises', COMMIT + '\ttgm_chan_park_promises')),
    ],
    'after_doorbell': [
        (COMMIT + PASS_ONE, PASS_ONE),
        (FENCE_KICK, FENCE_KICK + COMMIT),
    ],
    'before_checks': [
        (COMMIT + PASS_ONE, PASS_ONE),
        (PASS_ZERO, COMMIT + PASS_ZERO),
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
