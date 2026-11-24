"""SpringBoard's first life at boot.

backboardd arms its shell watchdog when SpringBoard checks in, and kills a SpringBoard that has not finished launching
70 s later ("no successful checkins since monitoring enabled by backboardd (70 seconds ago)"); the relaunched one then
dismisses the boot logo. backboardd's check-in line names the pid; SpringBoard's own "checkin finishedLaunching" does
not, so it belongs to the check-in it follows. Durations are mach times: the wall clock steps during boot (boot 2,
22 Nov: back 20 s between 22 and 33 s of uptime).
"""

from tools.devicerun.rules import FAIL, NO_DATA, PASS, REPORT, TIMELINE, Check, Result

OWNER = 'boot-springboard'
SOURCE = 'backboardd, SpringBoard and launchd of 24B119 (boot 2, 22 Nov)'
MACH_HZ = 19_200_000  # mach time counts the Tegra X1's system counter
WATCHDOG_S = 70.0
ICON_RESET_MAX_S = 0.5
CHECKIN = 'boot.springboard-checkin'
FINISHED = 'boot.springboard-finished'
LOGO = 'boot.springboard-logo-pid'
EXITED = 'boot.springboard-exited'
ANCHOR = 'boot.springboard-window-server'
ICON_RESET_START = 'boot.icon-reset-start'
ICON_RESET_DONE = 'boot.icon-reset-done'
ICON_STALL = 'boot.icon-stall'

SPRINGBOARD = [
    Check(
        ANCHOR,
        OWNER,
        'window server ready in ',
        REPORT,
        process='backboardd',
        source="backboardd's own startup line (com.apple.BackBoard:Common)",
    ),
    Check(
        CHECKIN,
        OWNER,
        'checkIn: <BKSystemShellClient: ',
        REPORT,
        process='backboardd',
        subsystem='com.apple.BackBoard',
        regex=r'; SpringBoard\.(?P<pid>\d+);',
        show=('pid',),
        source=SOURCE,
    ),
    Check(FINISHED, OWNER, 'checkin finishedLaunching', REPORT, process='SpringBoard', source=SOURCE),
    Check(
        LOGO,
        OWNER,
        'Dismissing boot logo (pid:',
        REPORT,
        process='backboardd',
        regex=r'\(pid:(?P<pid>\d+)\)',
        show=('pid',),
        source=SOURCE,
    ),
    Check(
        EXITED,
        OWNER,
        'job state = exited',
        REPORT,
        process='launchd',
        subsystem='/com.apple.SpringBoard [',
        source=SOURCE,
    ),
    Check(
        ICON_RESET_START,
        OWNER,
        'Resetting icon cache',
        REPORT,
        process='iconservicesagent',
        source='iconservicesagent of 24B119 (boot 2, 22 Nov)',
    ),
    Check(
        ICON_RESET_DONE,
        OWNER,
        'Cache reset complete:',
        REPORT,
        process='iconservicesagent',
        regex=r'removed (?P<items>\d+) items, (?P<failures>\d+) failures',
        show=('items', 'failures'),
        source='iconservicesagent of 24B119 (boot 2, 22 Nov)',
    ),
    Check(
        ICON_STALL,
        OWNER,
        'SwitchStall: SpringBoard[',
        REPORT,
        process='kernel',
        regex=(
            r'SwitchStall: SpringBoard\[\d+\] main thread 0x[0-9a-f]+ '
            r'.*?(?:waits for |owner \d+: |held for writing by )iconservicesagent\[\d+\]'
        ),
        source='kernel/switch/diag/stall*.c (debug kernels)',
    ),
]

# Debug kernels' and the Metal driver's lines for the waits the log could not split on boot 2 (CAUSES.md section 5)
OBSERVATIONS = [
    Check(
        'boot.stall-chain',
        OWNER,
        'SwitchStall: ',
        TIMELINE,
        process='kernel',
        regex=r' main thread 0x[0-9a-f]+ chain:',
        source='kernel/switch/diag/stall_chain.c (debug kernels)',
        note="a stalled UI main thread's owners past the first and the writer of each kernel rwlock on the way: who "
        "held the APFS lock backboardd's import thread waited 12.1 s on, whom wifid's and runningboardd's threads "
        'waited for (boot 2: not logged)',
    ),
    Check(
        'boot.steps',
        OWNER,
        'SwitchBootSteps: ',
        TIMELINE,
        process='kernel',
        source='kernel/switch/include/switch/boot_steps.h (debug kernels)',
        note='the cryptex setup (3.03-4.18 s on boot 2, 0.72 s of it with no line) and the steps before launchd '
        '(5.97-7.50 s, the Joy-Con rail pairing in it)',
    ),
    Check(
        'boot.bluetooth-before-launchd',
        OWNER,
        'SwitchBluetooth: before launchd: ',
        TIMELINE,
        process='kernel',
        source='drivers/SwitchBluetooth/SwitchBluetooth.cpp (debug kernels)',
        note="the staged address's read against the Joy-Con rails' hand-off",
    ),
    Check(
        'boot.tasks',
        OWNER,
        'SwitchBootTask: ',
        TIMELINE,
        process='kernel',
        source='kernel/switch/sched/cpu.c (debug kernels)',
        note="launchd's boot tasks by their exits until SpringBoard execs (9.70-16.39 s on boot 2)",
    ),
    Check(
        'boot.metal-open-split',
        OWNER,
        'SwitchMetal: open split in ',
        TIMELINE,
        process='backboardd',
        source='drivers/TegraGM20BMetal/TegraGM20BMetalPlugin.mm (debug builds)',
        note="the render server's Metal open (1366 ms on boot 2) by part",
    ),
    Check(
        'boot.keystore-slow',
        OWNER,
        'AppleKeyStore: selector ',
        REPORT,
        process='kernel',
        regex=r'from (?P<caller>\S+?)\[\d+\] took (?P<ms>\d+) ms',
        show=('caller',),
        source='drivers/NativeKeyStoreEvents/NativeKeyStoreEvents.cpp (debug kernels)',
        note="keystore calls of 20 ms or more: whether runningboardd's 1.7 s wait behind usermanagerd at 20.7 s on "
        'boot 2 is our stand-in',
    ),
]

FIRST_LIFE = Check(
    'boot.springboard-first-life',
    OWNER,
    'checkIn: <BKSystemShellClient: ',
    f'the first SpringBoard finishes launching < {WATCHDOG_S:g} s after its check-in and dismisses the logo',
    source=SOURCE,
    note='boot 2 (22 Nov): SpringBoard[30] killed 74 s after its check-in, SpringBoard[400] dismissed the logo and '
    'finished 21.3 s after its own',
)

ICON_RESET = Check(
    'boot.icon-reset-off-path',
    OWNER,
    'Cache reset complete:',
    f'icon cache reset finishes in < {ICON_RESET_MAX_S:g} s or removed 0 items with 0 pre-logo SpringBoard '
    'SwitchStall waits on iconservicesagent',
    source='iconservicesagent and SwitchStall of 24B119 (boot 2, 22 Nov)',
    note='boot 2 (22 Nov): iconservicesagent spent 9.72 s removing 1 salt folder on its main thread before opening '
    'its XPC listener, stalling SpringBoard[30] 11.0 s',
)


def seconds(ticks: float) -> float:
    return ticks / MACH_HZ


def in_boot(lines: list[dict], boot: str) -> list[dict]:
    return sorted((f for f in lines if f['boot'] == boot), key=lambda f: f['mach'])


def coalesce_checkins(checkins: list[dict]) -> list[dict]:
    """Keep the earliest check-in for each pid so a second checkIn line for the same SpringBoard does not split it."""
    lives: list[dict] = []
    seen: set[str] = set()
    for checkin in checkins:
        if checkin['pid'] not in seen:
            seen.add(checkin['pid'])
            lives.append(checkin)
    return lives


def life_text(checkin: dict, finished: float | None, next_checkin: dict | None) -> str:
    who = f'SpringBoard[{checkin["pid"]}]'
    if finished is not None:
        return f'{who} check-in to finishedLaunching {seconds(finished - checkin["mach"]):.1f} s'
    if next_checkin is not None:
        later = seconds(next_checkin['mach'] - checkin['mach'])
        return f'{who} never finished launching (SpringBoard[{next_checkin["pid"]}] checked in {later:.1f} s later)'
    return f'{who} never finished launching'


def judge_boot(boot: str, lines: dict[str, list[dict]]) -> tuple[str, str]:
    """Return (verdict, summary) for this boot's first SpringBoard life."""
    checkins_raw = in_boot(lines[CHECKIN], boot)
    finished_lines = in_boot(lines[FINISHED], boot)
    logos = in_boot(lines[LOGO], boot)
    exits_all = in_boot(lines[EXITED], boot)
    anchors = in_boot(lines[ANCHOR], boot)
    if any(f['mach'] <= 0 for f in (*checkins_raw, *finished_lines, *logos, *exits_all, *anchors)):
        return NO_DATA, f'boot {boot}: missing machTimestamp'
    logo = logos[0] if logos else None
    exits = [f for f in exits_all if logo is None or f['mach'] < logo['mach']]
    if not checkins_raw:
        if exits:
            tail = f'logo by SpringBoard[{logo["pid"]}]' if logo else 'no logo'
            return FAIL, f'boot {boot}: SpringBoard exited {len(exits)} time(s) without checking in, {tail}'
        return NO_DATA, f'boot {boot}: no backboardd check-in line'
    checkins = coalesce_checkins(checkins_raw)
    finished = [f['mach'] for f in finished_lines]
    lives, first_done = [], None
    for i, checkin in enumerate(checkins):
        nxt = checkins[i + 1] if i + 1 < len(checkins) else None
        end = nxt['mach'] if nxt else float('inf')
        done = next((t for t in finished if checkin['mach'] <= t < end), None)
        if i == 0:
            first_done = done
        lives.append(life_text(checkin, done, nxt))
    first = checkins[0]
    ok = (
        first_done is not None
        and seconds(first_done - first['mach']) < WATCHDOG_S
        and not exits
        and logo is not None
        and logo['pid'] == first['pid']
    )
    parts = [f'boot {boot}: ' + '; '.join(lives)]
    if exits:
        parts.append(f'SpringBoard exited {len(exits)} time(s) before the logo')
    parts.append(f'logo by SpringBoard[{logo["pid"]}]' if logo else 'no logo')
    if not ok:
        return FAIL, ', '.join(parts)
    if not any(a['mach'] < first['mach'] for a in anchors):
        parts.append('truncated start (no window-server-ready line before first check-in)')
        return NO_DATA, ', '.join(parts)
    return PASS, ', '.join(parts)


def describe_icon_resets(starts: list[dict], dones: list[dict]) -> tuple[list[str], bool, bool]:
    """Pair icon cache reset starts with completions; return (parts, all_ok, missing_start)."""
    parts: list[str] = []
    reset_ok = True
    missing_start = False
    matched_dones: set[int] = set()
    for i, start in enumerate(starts):
        nxt = starts[i + 1]['mach'] if i + 1 < len(starts) else float('inf')
        match = next(
            ((j, d) for j, d in enumerate(dones) if j not in matched_dones and start['mach'] <= d['mach'] < nxt),
            None,
        )
        if match is None:
            reset_ok = False
            parts.append('reset started without completion')
            continue
        j, done = match
        matched_dones.add(j)
        dur_s = seconds(done['mach'] - start['mach'])
        items, failures = int(done['items']), int(done['failures'])
        parts.append(f'reset {dur_s:.2f} s (removed {items} items, {failures} failures)')
        if failures != 0 or (dur_s >= ICON_RESET_MAX_S and items != 0):
            reset_ok = False
    for j, done in enumerate(dones):
        if j in matched_dones:
            continue
        items, failures = int(done['items']), int(done['failures'])
        parts.append(f'reset completed (removed {items} items, {failures} failures, no start line)')
        if failures != 0:
            reset_ok = False
        elif items != 0:
            missing_start = True
    return parts, reset_ok, missing_start


def judge_icon_reset(boot: str, lines: dict[str, list[dict]]) -> tuple[str, str]:
    """Return (verdict, summary) for whether this boot's icon cache reset stayed off SpringBoard's critical path."""
    starts = in_boot(lines[ICON_RESET_START], boot)
    dones = in_boot(lines[ICON_RESET_DONE], boot)
    stalls_all = in_boot(lines[ICON_STALL], boot)
    logos = in_boot(lines[LOGO], boot)
    if any(f['mach'] <= 0 for f in (*starts, *dones, *stalls_all, *logos)):
        return NO_DATA, f'boot {boot}: missing machTimestamp'
    logo = logos[0] if logos else None
    pre_stalls = [f for f in stalls_all if logo is None or f['mach'] < logo['mach']]
    if not starts and not dones:
        if pre_stalls:
            return FAIL, f'boot {boot}: {len(pre_stalls)} pre-logo SpringBoard SwitchStall wait(s) on iconservicesagent'
        return NO_DATA, f'boot {boot}: no icon cache reset'
    parts, reset_ok, missing_start = describe_icon_resets(starts, dones)
    parts.append(f'{len(pre_stalls)} pre-logo SwitchStall wait(s) on iconservicesagent')
    summary = f'boot {boot}: ' + ', '.join(parts)
    if pre_stalls or not reset_ok:
        return FAIL, summary
    if missing_start:
        return NO_DATA, summary
    return PASS, summary


def icon_reset_row(by_check: dict[str, Result]) -> Result:
    """Return the icon-cache-reset-off-path verdict across every boot with icon reset or stall lines."""
    lines = {
        name: by_check[name].matches if name in by_check else []
        for name in (ICON_RESET_START, ICON_RESET_DONE, ICON_STALL, LOGO)
    }
    boots = sorted({f['boot'] for name in (ICON_RESET_START, ICON_RESET_DONE, ICON_STALL) for f in lines[name]})
    if not boots:
        return Result(ICON_RESET, NO_DATA, 'no icon cache reset or SpringBoard iconservicesagent stall')
    judged = [judge_icon_reset(boot, lines) for boot in boots]
    verdicts = [v for v, _ in judged]
    verdict = FAIL if FAIL in verdicts else PASS if all(v == PASS for v in verdicts) else NO_DATA
    return Result(ICON_RESET, verdict, ' | '.join(text for _, text in judged))


def springboard_rows(by_check: dict[str, Result]) -> list[Result]:
    """Return the first-life and icon-reset-off-path verdicts of every boot with SpringBoard lifecycle lines."""
    lines = {
        name: by_check[name].matches if name in by_check else [] for name in (CHECKIN, FINISHED, LOGO, EXITED, ANCHOR)
    }
    boots = sorted({f['boot'] for name in (CHECKIN, FINISHED, LOGO, EXITED) for f in lines[name]})
    if not boots:
        first_life = Result(FIRST_LIFE, NO_DATA, "no backboardd check-in line (24B119's format)")
    else:
        judged = [judge_boot(boot, lines) for boot in boots]
        verdicts = [v for v, _ in judged]
        verdict = FAIL if FAIL in verdicts else PASS if all(v == PASS for v in verdicts) else NO_DATA
        first_life = Result(FIRST_LIFE, verdict, ' | '.join(text for _, text in judged))
    return [first_life, icon_reset_row(by_check)]
