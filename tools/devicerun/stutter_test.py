"""stutter.py's checks over lines in the formats the kernel logs (the 11-06 boot's, and the power line's)."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.rules import FAIL, NO_DATA, PASS
from tools.devicerun.stutter import STUTTER

STATS_1106 = (
    'SwitchDisplay stats: 5016 ms: 244 swaps 48.6/s, interval 20629/208431 client 10982/194799 submit-retire '
    '16180/60472 (queue 257/12089 flip-wait 0/0 map 0/0 gpu 0/0 cache 0/0 scan 0/0 copy 0/0 gpu-copy 6950/31846 '
    'read 0) us mean/max; copy fast 0 byte 0 gpu 244 (declined 0); vsync 273 sent 54.4/s of 279 vblanks, late '
    '46/2946 us; 0 gpu timeouts, 0 new maps, 0 underflow frames; attr source 0x100 target 0x44; present 232 (232 '
    'timed): interval p50 17 p99 152 p99.9 216 max 216.6 ms, frames 1:207 2:13 3:5 4+:7, >1.5x 25 >2x 12, gaps '
    '0/0; swap-to-scan p50 14 p99 37 p99.9 38 max 38.4 ms; frame 16.666 ms (16638-16698 us, 301), fe-late '
    '40/11368 us, folded 0, flip-waited 0, latch 1st 232 later 0, rejected 0 (reason 0)'
)
STATS_LOCKED = (
    'SwitchDisplay stats: 5000 ms: 300 swaps 60.0/s, interval 16666/16900 client 900/1500 submit-retire '
    '9000/11000 (queue 30/90 flip-wait 0/0 map 0/0 gpu 0/0 cache 0/0 scan 0/0 copy 0/0 gpu-copy 0/0 read 0) us '
    'mean/max; copy fast 0 byte 0 gpu 0 (declined 0); vsync 300 sent 60.0/s of 300 vblanks, late 40/210 us; 0 '
    'gpu timeouts, 0 new maps, 0 underflow frames; attr source 0x100 target 0x44; present 300 (300 timed): '
    'interval p50 17 p99 17 p99.9 17 max 16.9 ms, frames 1:300 2:0 3:0 4+:0, >1.5x 0 >2x 0, gaps 0/0; '
    'swap-to-scan p50 12 p99 14 p99.9 14 max 14.2 ms; frame 16.666 ms (16640-16690 us, 300), fe-late 35/240 us, '
    'folded 0, flip-waited 0, latch 1st 300 later 0, rejected 0 (reason 0)'
)
PACING_1106 = (
    'SwitchDisplay pacing: 219 presents: swap-to-scan = queue 182/7371 (surface wait 0/0) + copy 8296/19925 + '
    'latch 11091/16674 us mean/max; direct 223; 3 buffers, replaced 3, collapsed 0; planes on: flips 0 (b-only '
    '0) latched 0, maps 0, dropped 0, underflow 0, unready 0; long 51: idle 1/6 ca 1/4 gpu 4/4 queue 0/0 latch '
    '0/0 rate 45/68; vsync folded 0 late>1ms 2 idle 1; touch->swap 13240/59312 us mean/max'
)
POWER = (
    'SwitchPower: 10000 ms: cpu0 c7 41.0% (310, 0 skipped, 0 frame wakes) wfi 40.0% busy 19.0%, cpu1 c7 70.2% '
    '(120, 0 skipped, 0 frame wakes) wfi 20.0% busy 9.8%, cpu2 c7 80.0% (90, 1 skipped, 0 frame wakes) wfi 15.0% '
    'busy 5.0%, cpu3 c7 2.0% ({wakes}, 0 skipped, {wakes} frame wakes) wfi 90.0% busy 8.0%; c7 timer exits 520, '
    'mean 88 us, boot max {max} us, {late} late; EMC 6 moves 204:61.0% 1600:39.0%, {raises} raises: hint to top '
    '400/{emc} us (BPMP max 300 us); battery -900 mA x 4100 mV = -3690 mW'
)
RENDER = (
    'SwitchFrameRender: vsync 1266197624: woke 140 us after it; first submit 6330 us (cpu 5200, blocked 300 us '
    'in 2); swap 9730 us (cpu 8000, blocked 900 us in 4, waiting for a core {wait} us); longest block 600 us, '
    'user pc dsc 0x1a2b lr dsc 0x1a00, kernel continuation 0xfffffe0007123450; dropped 0'
)
FRAME = (
    'SwitchFrame: latch 1273234975 after 2 frames (33334 us), cause gpu; origin vsync 1272597613; first submit '
    '5768, last submit 5768 (1, exec 41 us), swap 6030, offer 6064, gpu done 17152, flip 17176, latch 33195 us; '
    'other submits 0; swap thread base 54 sched 54 mode 2 qos 0; cpu {mhz} MHz at vsync 1785 at swap; imp 0 0 us'
)


RATES = (
    'TegraGM20BMetal: backboardd raw 5s c=300(60.0/s) m2v=0 sw=0 cpu={cpu} acq=0 enc=120us/c d=9.0 p=2.0 b=0.10 '
    'defer=0 abort=0 rw=0 pool=16MB/4 grow=0 imp=12/40MB clean=0.0KB/c ce=0/0KB ev=0/0 ws={ws}/{wsus}us wc=0/0us '
    'sp=0/0'
)


def record(message: str, second: int = 0) -> dict:
    raw = message.startswith('TegraGM20BMetal: ')
    return {
        'timestamp': f'2026-11-08 09:00:{second:02d}.000000+0000',
        'bootUUID': 'BOOTA000',
        'processImagePath': '/usr/libexec/backboardd' if raw else '/System/Library/Kernels/kernel.development',
        'subsystem': 'com.brunostjohn.switch.TegraGM20BMetal' if raw else '',
        'category': '',
        'messageType': 'Default',
        'eventMessage': message,
    }


def verdicts(*messages: str) -> dict[str, str]:
    results, _ = devicerun.run_checks([record(m, i) for i, m in enumerate(messages)], STUTTER)
    return {r.check.name: r.verdict for r in results}


class Stutter(unittest.TestCase):
    def test_the_11_06_session_fails_where_it_stuttered(self):
        got = verdicts(STATS_1106, PACING_1106)
        self.assertEqual(got['stutter.vsync-delivered'], FAIL)
        self.assertEqual(got['stutter.vsync-late'], FAIL)
        self.assertEqual(got['stutter.frame-end-late'], FAIL)
        self.assertEqual(got['stutter.vsync-late-1ms'], FAIL)
        self.assertEqual(got['stutter.vsync-folded'], PASS)
        self.assertEqual(got['stutter.long-ca'], FAIL)
        self.assertEqual(got['stutter.long-gpu'], FAIL)
        self.assertEqual(got['stutter.long-queue'], PASS)
        self.assertEqual(got['stutter.c7-frame-wakes'], NO_DATA)

    def test_a_locked_window_passes(self):
        got = verdicts(STATS_LOCKED)
        self.assertEqual(got['stutter.vsync-delivered'], PASS)
        self.assertEqual(got['stutter.vsync-late'], PASS)
        self.assertEqual(got['stutter.frame-end-late'], PASS)

    def test_c7_and_the_emc_raise(self):
        got = verdicts(POWER.format(wakes=1, max=126, late=0, raises=3, emc=900))
        self.assertEqual(got['stutter.c7-frame-wakes'], PASS)
        self.assertEqual(got['stutter.emc-raise'], PASS)
        self.assertEqual(got['stutter.c7-exit-max'], PASS)
        self.assertEqual(got['stutter.c7-exit-late'], PASS)
        got = verdicts(POWER.format(wakes=40, max=410, late=3, raises=3, emc=5200))
        self.assertEqual(got['stutter.c7-frame-wakes'], FAIL)
        self.assertEqual(got['stutter.emc-raise'], FAIL)
        self.assertEqual(got['stutter.c7-exit-max'], FAIL)
        self.assertEqual(got['stutter.c7-exit-late'], FAIL)

    def test_render_thread_and_clocks(self):
        got = verdicts(RENDER.format(wait=150), FRAME.format(mhz=1785))
        self.assertEqual(got['stutter.render-core-wait'], PASS)
        self.assertEqual(got['stutter.cpu-at-vsync'], PASS)
        got = verdicts(RENDER.format(wait=4100), FRAME.format(mhz=1020))
        self.assertEqual(got['stutter.render-core-wait'], FAIL)
        self.assertEqual(got['stutter.cpu-at-vsync'], FAIL)

    def test_gpu_raise_line(self):
        line = (
            'tegra-gm20b: idle clock 10000 ms: 61.2% at 76800 kHz, 4 drops, 4 raises (2 by a hint), raise mean '
            '{mean} ns max {max} ns; 0 in flight; now 768000 kHz'
        )
        self.assertEqual(verdicts(line.format(mean=9000, max=14000))['stutter.gpu-raise'], PASS)
        self.assertEqual(verdicts(line.format(mean=9000, max=250000))['stutter.gpu-raise'], FAIL)

    def test_render_thread_cpu_waits_on_the_raw_path(self):
        got = verdicts(RATES.format(cpu=0, ws=300, wsus=900))
        self.assertEqual(got['stutter.raw-scheduled-wait'], PASS)
        self.assertEqual(got['stutter.raw-completed-wait'], PASS)
        self.assertEqual(got['stutter.raw-cpu-ordered'], PASS)
        self.assertEqual(got['stutter.raw-ring-waits'], PASS)
        self.assertEqual(got['stutter.raw-pool-stalls'], PASS)
        got = verdicts(RATES.format(cpu=4, ws=300, wsus=48000))
        self.assertEqual(got['stutter.raw-scheduled-wait'], FAIL)
        self.assertEqual(got['stutter.raw-cpu-ordered'], FAIL)


if __name__ == '__main__':
    unittest.main()
