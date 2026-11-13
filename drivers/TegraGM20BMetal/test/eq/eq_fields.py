"""Normalise one driver's nvdec log of a frame into the draws and clears eq compares.

Every submission's events are one frame (a command buffer the raw path submits in segments is several submissions,
NVK's one), and each draw's fields drop what two drivers compile or lay out their own way:

  draw, vb*, ib, tsc*/tic*, rt*/zt   the draw's parameters, vertex bytes, header-0 samplers and targets, the targets
                                     named in order of first appearance in draws' target lines
  R                                  its live registers, less the pools' limits and what a program decides (register
                                     counts, the bound-texture slot)
  stage<n>                           per stage: the contents its constant buffers and their descriptors reach (a set,
                                     whatever slot or set offset holds them), its root draw parameters and push words
"""

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'raw/pack'))
import eqcheck

# SET_PIPELINE_REGISTER_COUNT(i) and SET_BINDLESS_TEXTURE: each driver's compiler decides them
PROGRAM_DERIVED = {f'{0x200C + 0x40 * i:x}' for i in range(6)} | {'2608'}
CONTENT = re.compile(r'(?:data=|ptr\{)([0-9a-f]+:[0-9a-f]+)')
HANDLE = re.compile(r'handle\{[^}]*\}')


@dataclass
class Stage:
    content: set[str] = field(default_factory=set)
    app: set[str] = field(default_factory=set)  # its uniform buffers' (nvk_cbuf_type 5) contents
    set: str = ''  # the set-0 upload its constant buffer of nvk_cbuf_type 3 binds: its words' hash
    draw: str = ''
    push: list[str] = field(default_factory=list)


@dataclass
class Draw:
    fields: dict
    registers: dict[str, str]
    stages: dict[str, Stage]
    barriers: set[str]
    first: bool = False  # a pass's first: after a clear or on other targets than the draw before

    def box(self) -> tuple[int, int, int, int]:
        """Give the pixels the draw may touch: its scissor (x0, x1, y0, y1), or the whole target with the test off."""
        if self.registers.get('e00', '0') == '0':
            return (0, 1 << 16, 0, 1 << 16)
        h, v = int(self.registers.get('e04', '0'), 16), int(self.registers.get('e08', '0'), 16)
        return (h & 0xFFFF, h >> 16, v & 0xFFFF, v >> 16)

    def writes_color(self) -> bool:
        """Say whether the draw writes a colour: through the ROP (SET_CT_WRITE) or from its program in the interlock."""
        ct = any(self.registers.get(f'{0x1A00 + 4 * j:x}', '0') != '0' for j in range(8))
        return ct or self.registers.get('1224', '0') != '0'


def target_names(events: list[dict]) -> dict[str, str]:
    names = {}
    for e in events:
        if e['head'].startswith('DRAW'):
            for line in e['lines']:
                if re.match(r'(rt\d+|zt)=', line):
                    for r in re.findall(r'\bR\d+\b', line):
                        names.setdefault(r, f'N{len(names) + 1}')
    return names


def renamed(text: str, names: dict[str, str]) -> str:
    return re.sub(r'\bR\d+\b', lambda m: names.get(m.group(0), 'X'), text)


def stages(lines: list[str]) -> dict[str, Stage]:
    out, current = {}, None
    for line in lines:
        if line.startswith('stage'):
            current = out.setdefault(line.split()[0], Stage())
        elif current is None:
            continue
        elif line.startswith('cb'):
            current.content.update(CONTENT.findall(line))
            current.content.update(HANDLE.findall(line))
            if ' type=5 ' in line:
                current.app.update(re.findall(r'data=(\S+)', line))
            if ' type=3 ' in line and (words := re.search(r'data=(\S+)', line)):
                current.set = words.group(1)
            if m := re.search(r'root draw=(\S+)', line):
                current.draw = m.group(1)
        elif line.startswith('rootwords'):
            current.push = line.split(' push:')[1].split()
    return out


def draw_of(event: dict) -> Draw:
    f = eqcheck.fields(event)
    kept = {k: v for k, v in f.items() if re.match(r'(draw|vb\d+|ib|tsc\d+|tic\d+|rt\d+|zt)$', k)}
    registers = {m: v for m, v in f.get('R', {}).items() if m not in eqcheck.POOL_LIMITS | PROGRAM_DERIVED}
    return Draw(kept, registers, stages(event['lines']), f['barriers'])


@dataclass
class Frame:
    draws: list[Draw]
    early: list[str]  # the clears before its first draw (eqcheck.split_depth_stencil)
    clears: list[str]  # the others


def frame(log: str) -> Frame:
    """Read the log's draws and clears, every submission's as one frame."""
    events = [e for submission in eqcheck.frames(log) for e in submission]
    names = target_names(events)
    out, early, clears, cleared = Frame([], [], []), [], [], True
    for e in events:
        e['head'] = renamed(e['head'], names)
        e['lines'] = [renamed(line, names) for line in e['lines']]
        if e['head'].startswith('CLEAR'):
            (clears if out.draws else early).append(eqcheck.fields(e)['clear'])
            cleared = True
        elif e['head'].startswith('DRAW'):
            d = draw_of(e)
            targets = {k: v for k, v in d.fields.items() if k.startswith(('rt', 'zt'))}
            d.first = (
                cleared
                or not out.draws
                or targets != {k: v for k, v in out.draws[-1].fields.items() if k.startswith(('rt', 'zt'))}
            )
            out.draws.append(d)
            cleared = False
    out.early, out.clears = eqcheck.split_depth_stencil(early), eqcheck.split_depth_stencil(clears)
    return out


def digest_fields(d: Draw) -> dict[str, str]:
    """Give what a golden holds of a draw: each field's text, its registers and each stage's reach as one each."""
    out = {k: str(v) for k, v in sorted(d.fields.items())}
    out['R'] = ' '.join(f'{m}={v}' for m, v in sorted(d.registers.items()))
    for name, s in sorted(d.stages.items()):
        out[name] = ' '.join(sorted(s.content)) + f' draw={s.draw} push={",".join(s.push)}'
    return out
