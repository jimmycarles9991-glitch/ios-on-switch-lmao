#!/usr/bin/env python3
"""Read capture/probe.out + probe.log (nvdec NVDEC_PROBE records) into per-probe dicts."""


def load(out_path: str, log_path: str) -> list[dict]:
    """Give each PROBE line of probe.out with its submission's decoded draw (probe.mm: one command buffer per state).

    The probes are the log's last submissions: how many NVK makes before them varies (where the device exposes the pixel
    interlock, its ticket buffer is cleared by a copy-engine submission of its own at device creation). A probe whose
    command buffer m2v failed (its FAILED line, the error in p['failed']) submitted nothing and has no records.
    """
    probes = _probes(out_path)
    drawn = [p for p in probes if 'failed' not in p]
    subs = _submissions(log_path)
    first = len(subs) - len(drawn)
    if first < 0:
        raise ValueError(f'{log_path}: {len(subs)} submissions for {len(drawn)} probes')
    for p in probes:
        if 'failed' in p:
            p.update(draws=0, dispatches=0, clears=[], canon=[], records=[])
    for n, p in enumerate(drawn):
        s = subs[first + n]
        p['draws'] = sum(1 for line in s if line.startswith('DRAW'))
        p['dispatches'] = sum(1 for line in s if line.startswith('DISPATCH'))
        p['clears'] = [line for line in s if line.startswith('CLEAR')]
        p['canon'] = s
        p['records'] = _records(s)
        _check_tags(p, log_path)
    return probes


def _check_tags(p: dict, log_path: str) -> None:
    """Fail where a probe's draw reads another probe's tagged payload: its records are another state's."""
    for rec in p['records']:
        for key, (_, words) in (rec or {}).get('ptr', {}).items():
            if len(words) > 1 and words[0] >> 16 == 0xC0DE and words[1] != p['index']:
                raise ValueError(
                    f"{log_path}: probe {p['index']} reads probe {words[1]}'s payload at {key}: records misaligned"
                )


def _probes(out_path: str) -> list[dict]:
    probes = []
    with open(out_path) as out:
        for line in out:
            if line.startswith('PROBE '):
                _, i, key = line.rstrip('\n').split(' ', 2)
                probes.append({'index': int(i), 'key': key.strip()})
            elif line.startswith('FAILED ') and probes:
                _, i, error = line.rstrip('\n').split(' ', 2)
                if probes[-1]['index'] == int(i):
                    probes[-1]['failed'] = error
    return probes


def _submissions(log_path: str) -> list[list[str]]:
    subs, cur = [], []
    with open(log_path) as log:
        for text in log:
            line = text.rstrip('\n')
            cur.append(line)
            if line.startswith('SUBMIT'):
                subs.append(cur)
                cur = []
    return subs


def _records(s: list[str]) -> list[dict | None]:
    recs, rec = [], None
    for line in s:
        if line == 'P begin':
            rec = {'binds': {}, 'ptr': {}, 'tic': {}, 'tsc': {}, 'vb': {}, 'vbinfo': {}, 'prog': {}}
        elif line == 'P end':
            recs.append(rec)
            rec = None
        elif rec is not None and line.startswith('P '):
            _record_binding(rec, line.split())
    return recs


def _record_binding(rec: dict, f: list[str]) -> None:
    t = f[1]
    if t == 'regs':
        rec['regs'] = {int(a, 16): int(b, 16) for a, b in (x.split('=') for x in f[2:])}
    elif t == 'bind':
        rec['binds'][(int(f[2]), int(f[3]))] = (int(f[4], 16), int(f[5], 16))
    elif t in ('root', 'set0', 'set1'):
        rec[t] = (int(f[2], 16), [int(x, 16) for x in f[3:]])
    elif t.startswith(('ptr', 'pushptr')):
        rec['ptr'][t] = (int(f[2], 16), [int(x, 16) for x in f[3:]])
    else:
        _record_state(rec, f)


def _record_state(rec: dict, f: list[str]) -> None:
    t = f[1]
    if t == 'tic':
        rec['tic'][int(f[2])] = (f[3], [int(x, 16) for x in f[4:]])
    elif t == 'tsc':
        rec['tsc'][int(f[2])] = [int(x, 16) for x in f[3:]]
    elif t == 'vbinfo':
        rec['vbinfo'][int(f[2])] = (int(f[3], 16), int(f[4], 16), int(f[5], 16))
    elif t.startswith('vb'):
        rec['vb'][int(t[2:])] = (int(f[2], 16), [int(x, 16) for x in f[3:]])
    elif t == 'prog':
        rec['prog'][int(f[2])] = (f[3], int(f[4], 16), f[5])
    elif t == 'draw':
        rec['draw'] = (int(f[2]), int(f[3]), int(f[4], 16))
    elif t == 'pushw':
        rec['pushw'] = f[2:]
