"""The capability table's correctness backlog.

The rows graded below chip, and every place the current device answers (raw_device_answers.h, per iOS kind) or the raw
path's format tables (raw_gpu.cpp kFormats, raw_zeta.h) disagree with the table. Disagreements are ordered by how wrong
the current answer is: an over-claim (the device promises what the hardware lacks) first, then a wrong value, then an
under-claim (the hardware does it, the device says no or sends it to m2v), then a format the raw path lowers to another
format's codes. Within a class, the larger mismatch first.

    bazel run //kits/SwitchGM20B:caps_report
"""

import math
import os
import re
import sys

import caps
import gen_caps

ORDER = ['over-claim', 'wrong value', 'under-claim', 'lowered']
RAW = 'drivers/TegraGM20BMetal/raw/runtime/'


def answers(text: str, chip_features: dict | None = None) -> dict:
    """The answers per kind; an answer given as chipFeature(SWITCH_GM20B_FEATURE_<id>) reads `chip_features`."""
    kinds = {}
    for m in re.finditer(r'kCounts(\w+)\[\] = \{(.*?)\};', text, re.DOTALL):
        counts = {s: int(v) for s, v in re.findall(r'\{"([\w:]+)", (\d+), (?:true|false)\}', m.group(2))}
        for s, row in re.findall(
            r'\{"([\w:]+)", chipFeature\(SWITCH_GM20B_FEATURE_(\w+)\), (?:true|false)\}', m.group(2)
        ):
            counts[s] = (chip_features or {}).get(row, 0)
        kinds.setdefault(m.group(1), {})['counts'] = counts
    for m in re.finditer(r'kArgs(\w+)\[\] = \{(.*?)\};', text, re.DOTALL):
        ranges = [
            (s, int(a), int(b), int(v))
            for s, a, b, v in re.findall(r'\{"([\w:]+)", (?:true|false), (\d+), (\d+), (\d+)\}', m.group(2))
        ]
        kinds.setdefault(m.group(1), {})['args'] = ranges
    return kinds


def chip_feature_answers(table: caps.Caps) -> dict:
    """raw_device_answers.h chipFeature's value of each feature row, by its SWITCH_GM20B_FEATURE_ name."""
    return {
        gen_caps.ident(r.cols['id']): int(r.cols['present'] == 'yes' and r.cols['grade'] == 'chip')
        for r in table.features
    }


def raw_formats(gpu_cpp: str, zeta_h: str) -> dict:
    rows = {}
    body = gpu_cpp[gpu_cpp.index('kFormats[] = {') :]
    body = body[: body.index('};')]
    for m in re.finditer(r'\{(\d+), (0x[0-9a-f]+|0), (0x[0-9a-f]+), (\d+), (?:true|false)', body):
        rows[int(m.group(1))] = {'ct': int(m.group(2), 0), 'tic0': int(m.group(3), 16), 'bytes': int(m.group(4))}
    for m in re.finditer(
        r'\{(\d+), (0x[0-9a-f]+), (\d+), (0x[0-9a-f]+), (?:true|false), (?:true|false), (0x[0-9a-f]+)\}', zeta_h
    ):
        rows[int(m.group(1))] = {'zt': int(m.group(2), 16), 'tic0': int(m.group(5), 16), 'bytes': int(m.group(3))}
    return rows


def size(answer: float, hw: float) -> float:
    if answer <= 0 or hw <= 0:
        return 99.0
    return abs(math.log2(answer / hw))


def compare_limits(table: caps.Caps, kind: str, ans: dict, out: list) -> None:
    counts, ranges = ans.get('counts', {}), ans.get('args', [])
    for row in table.limits:
        c = row.cols
        for sel in [s for s in c['metal'].split(',') if s != '-']:
            if c['bound'] == 'set':
                hw = {int(v) for v in c['value'].split(',')}
                got = {n for s, a, b, v in ranges if s == sel and v for n in range(a, b + 1)} & set(range(1, 17))
                if not got:
                    continue
                if got - hw:
                    out.append(('over-claim', 9, kind, f'{sel} answers {sorted(got)}, the hardware {sorted(hw)}'))
                if hw - got:
                    out.append(
                        (
                            'under-claim',
                            len(hw - got),
                            kind,
                            f'{sel} answers {sorted(got)}, the hardware renders {sorted(hw)} ({c["grade"]})',
                        )
                    )
                continue
            values = [v for s, a, b, v in ranges if s == sel] or ([counts[sel]] if sel in counts else [])
            if not values:
                continue
            hw = int(c['value'])
            answer = min(values) if c['bound'] == 'min' else max(values)
            over = answer < hw if c['bound'] == 'min' else answer > hw
            if answer == hw:
                continue
            label = 'over-claim' if over else 'under-claim'
            out.append(
                (
                    label,
                    size(answer, hw),
                    kind,
                    f"{sel} answers {answer}, the hardware's {c['name']} is {hw} ({c['grade']}, {c['hwtest']})",
                )
            )


def compare_features(table: caps.Caps, kind: str, ans: dict, out: list) -> None:
    counts = ans.get('counts', {})
    for row in table.features:
        c = row.cols
        if c['metal'] == '-':
            continue
        sel, _, want = c['metal'].partition('=')
        hw = c['present'] == 'yes' and want == '1'
        got = counts.get(sel)
        if got is None:
            out.append(
                (
                    'under-claim',
                    0.5,
                    kind,
                    f"{sel} has no raw answer (m2v's): the hardware has it ({c['id']}, {c['grade']}, {c['hwtest']})",
                )
            )
        elif got and not hw:
            out.append(('over-claim', 9, kind, f'{sel} answers yes, the hardware has no {c["id"]}'))
        elif not got and hw:
            out.append(
                (
                    'under-claim',
                    1,
                    kind,
                    f'{sel} answers no, the hardware has it ({c["id"]}, {c["grade"]}, {c["hwtest"]})',
                )
            )


def compare_formats(rows: list[dict], raw: dict, out: list) -> None:
    by_mtl = {r['mtl']: r for r in rows}
    for mtl, r in sorted(raw.items()):
        cap = by_mtl.get(mtl)
        name = cap['name'] if cap else str(mtl)
        if cap is None:
            out.append(('wrong value', 1, 'raw', f"format {mtl} is in raw's table, not in formats.tsv"))
            continue
        if cap['pipe'] == '-':
            out.append(
                (
                    'lowered',
                    1,
                    'raw',
                    (
                        f'{name}: no native target or texture header; raw stores and samples '
                        f"another format's codes (target 0x{r.get('ct', 0):02x})"
                    ),
                )
            )
            continue
        target = 'zt' if 'zt' in r else 'ct'
        if r.get(target) and r[target] != cap[target]:
            out.append(
                (
                    'wrong value',
                    2,
                    'raw',
                    f"{name}: raw's {target} 0x{r[target]:02x}, the hardware's 0x{cap[target]:02x}",
                )
            )
        if target == 'ct' and r['ct'] and 'render' not in cap['flags']:
            out.append(('over-claim', 9, 'raw', f'{name}: raw renders it, the hardware has no target for it'))
        if target == 'ct' and not r['ct'] and 'render' in cap['flags']:
            out.append(
                (
                    'under-claim',
                    1,
                    'raw',
                    (
                        f'{name}: raw samples it only, the hardware renders and '
                        f'{"blends" if "blend" in cap["flags"] else "stores"} it'
                    ),
                )
            )
        if r['tic0'] != cap['tic0']:
            out.append(
                (
                    'wrong value',
                    2,
                    'raw',
                    f"{name}: raw's texture header word 0 0x{r['tic0']:08x}, NIL's 0x{cap['tic0']:08x}",
                )
            )
    for cap in rows:
        if cap['mtl'] not in raw and ('texture' in cap['flags'] or 'render' in cap['flags']):
            what = 'renders and samples' if 'render' in cap['flags'] else 'samples'
            out.append(('under-claim', 0.75, 'raw', f'{cap["name"]}: the hardware {what} it, raw has no row (m2v)'))


def below_chip(table: caps.Caps) -> list[str]:
    lines = []
    for name in ('formats', 'limits', 'features'):
        rows = getattr(table, name)
        key = {'formats': 'name', 'limits': 'name', 'features': 'id'}[name]
        by = {g: [r.cols[key] for r in rows if r.cols['grade'] == g] for g in caps.GRADES}
        lines.append(f'{name}.tsv: {len(rows)} rows: ' + ', '.join(f'{g} {len(by[g])}' for g in reversed(caps.GRADES)))
        lines.extend(f'  {g}: ' + ', '.join(by[g]) for g in ('host', 'source') if by[g])
    lines.append(f'methods.tsv: {len(table.methods)} class methods and QMD fields, each under a feature row')
    return lines


def mapped_selectors(table: caps.Caps) -> set[str]:
    names = {s for r in table.limits for s in r.cols['metal'].split(',') if s != '-'}
    return names | {r.cols['metal'].partition('=')[0] for r in table.features if r.cols['metal'] != '-'}


def unmapped(table: caps.Caps, kinds: dict) -> list[str]:
    """List the device's answers no row speaks for, per kind: they are not compared."""
    mapped = mapped_selectors(table)
    lines = []
    for kind, ans in sorted(kinds.items()):
        selectors = set(ans.get('counts', {})) | {s for s, *_ in ans.get('args', [])}
        left = sorted(selectors - mapped)
        lines.append(f'{kind}: {len(selectors) - len(left)} of {len(selectors)} answers compared; not compared: ')
        lines[-1] += ', '.join(left)
    return lines


def compare_amplification(kind: str, ans: dict, out: list) -> None:
    """supportsVertexAmplificationCount: past maxVertexAmplificationCount promises counts the device itself denies."""
    count = ans.get('counts', {}).get('maxVertexAmplificationCount')
    ranges = [b for s, a, b, v in ans.get('args', []) if s == 'supportsVertexAmplificationCount:' and v]
    if count is not None and ranges and max(ranges) > count:
        out.append(
            (
                'over-claim',
                2,
                kind,
                (
                    f'supportsVertexAmplificationCount: answers yes up to {max(ranges)}, '
                    f'maxVertexAmplificationCount is {count}'
                ),
            )
        )


def main() -> None:
    root = os.environ.get('BUILD_WORKSPACE_DIRECTORY')
    args = gen_caps.runfiles_args(root)
    try:
        table, rows = gen_caps.build(args)
    except caps.CapsError as e:
        caps.fail(e)
    with open(RAW + 'raw_device_answers.h', encoding='utf-8') as f:
        kinds = answers(f.read(), chip_feature_answers(table))
    with open(RAW + 'raw_gpu.cpp', encoding='utf-8') as f, open(RAW + 'raw_zeta.h', encoding='utf-8') as z:
        raw = raw_formats(f.read(), z.read())
    found = []
    for kind, ans in sorted(kinds.items()):
        compare_limits(table, kind, ans, found)
        compare_features(table, kind, ans, found)
        compare_amplification(kind, ans, found)
    compare_formats(rows, raw, found)
    found.sort(key=lambda d: (ORDER.index(d[0]), -d[1], d[2], d[3]))
    out = ['# GM20B capability table', '', *below_chip(table)]
    out += ['', f'# Disagreements with the current device ({len(found)})', '']
    out += [f'{label:12} {kind:7} {text}' for label, _, kind, text in found]
    out += ['', '# Device answers no row compares', '', *unmapped(table, kinds)]
    if root:
        out += ['', 'Every cited file and line exists.']
    sys.stdout.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
