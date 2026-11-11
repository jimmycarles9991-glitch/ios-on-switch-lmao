#!/usr/bin/env python3
"""The raw path's split draws (runtime/raw_pack.h PackSplit) folded back into the draws NVK made, for eqcheck.py.

A split draw runs the split per-pixel program of NVK's fragment program and reads one more constant buffer, the
frontier, which a prologue dispatch at the submission's start wrote (nvdec prints the dispatch's program, the push words
and uniform buffers it read, and the draw's frontier names the dispatch that wrote it). Such a draw is the same GPU work
as NVK's when its program is the split of NVK's, its frontier was written by that split's prologue, and the prologue
read the push words and the uniform buffers the draw itself binds: NIR's constant folder holds the two programs to the
whole one (Mesa nvk_switch_split_proof.c). Its program and frontier lines are then NVK's, and the prologue dispatches
are taken out.
"""

import json
import os
import re

FRONTIER = re.compile(r'cb(\d+) frontier size=\w+ (.*)$')
UBO = re.compile(r'cb\d+ type=5 size=\w+ data=(\S+)$')


def load(path: str) -> list[dict]:
    """Give every packed split, naming its programs as nvdec does (pack.json)."""
    if not path or not os.path.exists(path):
        return []
    with open(path) as f:
        pack = json.load(f)
    ids = [p.get('name', p['id']) for p in pack['programs']]
    return [
        {'whole': ids[s['whole']], 'fs': ids[s['fs']], 'prologue': ids[s['prologue']], 'ubos': s['ubo_cbufs']}
        for s in pack.get('splits', [])
    ]


def stage_lines(ev: dict, stage: str) -> dict[str, str]:
    """Give a draw's lines of one stage by their key (stage5, cb0 root push, cb3, ...)."""
    out, current = {}, None
    for line in ev['lines']:
        if line.startswith('stage'):
            current = line.split()[0]
            if current == stage:
                out['stage'] = line
        elif current == stage and line.startswith('cb'):
            parts = line.split()
            out[parts[0] + (' ' + parts[2].split('=')[0] if parts[1] == 'root' else '')] = line
    return out


def check(ev: dict, split: dict, launches: set[str]) -> str | None:
    """Return why a split draw is not NVK's draw, or None."""
    lines = stage_lines(ev, 'stage5')
    frontier = next((m for m in map(FRONTIER.match, (x.strip() for x in ev['lines'])) if m), None)
    if not frontier:
        return 'no frontier'
    launch = frontier.group(2)
    if launch not in launches:
        return f'a frontier no prologue of its submission wrote ({launch})'
    fields = dict(x.split('=', 1) for x in launch.split())
    if fields.get('prologue') != split['prologue']:
        return f'prologue {fields.get("prologue")}, its split names {split["prologue"]}'
    push = lines.get('cb0 push', '').split('push=')[-1]
    if fields.get('push') != push:
        return f'its prologue read push words {fields.get("push")}, the draw {push}'
    for k, slot in enumerate(split['ubos']):
        ubo = UBO.match(lines.get(f'cb{slot}', ''))
        if not ubo or fields.get(f'cb{k + 1}') != ubo.group(1):
            return f'its prologue read uniform buffer {k + 1} as {fields.get(f"cb{k + 1}")}, the draw cb{slot} as {ubo}'
    return None


def fold(frames_of: list[list[dict]], splits: list[dict]) -> tuple[int, list[str]]:
    """Give each split draw its whole program and no frontier, and take the prologue dispatches out.

    Return the split draws and why each that is not NVK's draw is not.
    """
    # wholes whose per-pixel parts compile alike share one per-pixel program (splits.py per_pixel)
    by_fs: dict[str, list] = {}
    for s in splits:
        by_fs.setdefault(s['fs'], []).append((s['whole'], s))
    count, failed = 0, []
    for fi, events in enumerate(frames_of):
        launches = {
            line.strip()[len('split ') :].rsplit(' frontier=', 1)[0]
            for e in events
            if e['head'].startswith('DISPATCH')
            for line in e['lines']
            if line.strip().startswith('split ')
        }
        events[:] = [e for e in events if not any(x.strip().startswith('split ') for x in e['lines'])]
        for di, ev in enumerate(e for e in events if e['head'].startswith('DRAW')):
            stage = stage_lines(ev, 'stage5').get('stage', '')
            prog = re.search(r'prog=(\S+)', stage)
            if not prog or prog.group(1) not in by_fs:
                continue
            count += 1
            checked = [(whole, check(ev, split, launches)) for whole, split in by_fs[prog.group(1)]]
            whole, why = next(((w, None) for w, y in checked if y is None), checked[0])
            if why:
                failed.append(f'frame {fi} draw {di}: {why}')
                continue
            ev['lines'] = [
                re.sub(r'prog=\S+ regs=\S+', f'prog={whole} regs=*', x) if x.startswith('stage5') else x
                for x in ev['lines']
                if not FRONTIER.match(x.strip())
            ]
    return count, failed
