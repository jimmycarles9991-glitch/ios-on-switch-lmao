"""Tests for cpu.py and kprof.py parsers, symbolization, and pinned-window aggregation."""

import unittest
from datetime import UTC, datetime

from tools.perf import cpu, symbols
from tools.perf.source import Rec


def sched(msg: str, at: datetime) -> Rec:
    return Rec(
        at,
        0,
        proc='kernel',
        msg=msg,
        boot='B',
        pid=0,
        tid=1,
        subsystem='',
        category='',
        name='',
        kind='',
        ident=0,
    )


def _kernel_syms() -> symbols.Symbols:
    table = symbols.build_table(
        [
            symbols.Symbol(0xFFFFFFF007DD3CD0, '_SwitchCryptex_finishRequest'),
            symbols.Symbol(0xFFFFFFF007EF8F70, '_timer_callout_hot'),
            symbols.Symbol(0xFFFFFFF007123450, '_vm_fault_enter'),
            symbols.Symbol(0xFFFFFFF007120000, '_vm_fault_internal'),
            symbols.Symbol(0xFFFFFFF007A12340, '_NativeSwitchBlock_worker'),
            symbols.Symbol(0xFFFFFFF007111110, '_lck_spin_unlock'),
            symbols.Symbol(0xFFFFFFF007222220, '_thread_block_reason'),
        ]
    )
    return symbols.Symbols(kernel=table)


class CoreWaitsTest(unittest.TestCase):
    # 25 Nov: backboardd's main thread had no run for two windows, then one 32.6 s wait ending in the third.
    def test_a_wait_that_began_windows_earlier_keeps_its_start(self):
        recs = [
            sched(
                'SwitchSched 1/2 (10000 ms): backboardd.main[69] 53/53 TS IN FG: 10 runs 64/512/900 us',
                datetime(2026, 11, 25, 19, 16, 42, tzinfo=UTC),
            ),
            sched(
                'SwitchSched 1/2 (10000 ms): backboardd.main[69] 53/53 TS IN FG: 12 runs 64/32555950/32555950 us, '
                'longest began 22555 ms before the window; '
                'display.primary[53] 54/54 FX - FIX: 600 runs 32/64/90 us',
                datetime(2026, 11, 25, 19, 17, 12, tzinfo=UTC),
            ),
        ]
        waits = cpu.core_waits(recs)
        main = waits['backboardd.main']
        self.assertEqual((main.runs, main.worst_us, main.began_before_ms), (22, 32555950, 22555))
        self.assertEqual(main.when, datetime(2026, 11, 25, 19, 17, 12, tzinfo=UTC))
        self.assertEqual((waits['display.primary'].worst_us, waits['display.primary'].began_before_ms), (90, 0))


class CpuTelemetryTest(unittest.TestCase):
    def test_power_irqs_systime_and_callouts_parse_and_symbolize(self):
        t = datetime(2026, 11, 26, 19, 38, 42, tzinfo=UTC)
        recs = [
            sched(
                'SwitchPower: 10000 ms: cpu0 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 0.0% busy 100.0%, '
                'cpu1 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 0.0% busy 100.0%, '
                'cpu2 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 0.0% busy 100.0%, '
                'cpu3 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 0.0% busy 100.0%; '
                'c7 timer exits 0, mean 0 us, boot max 0 us, 0 late; '
                'irq cpu0 3800 timer 16000 ipi 500 spi, cpu1 3900 timer 15000 ipi 10500 spi, '
                'cpu2 3800 timer 15000 ipi 250 spi, cpu3 3700 timer 14000 ipi 150 spi; EMC 0 moves 1600:100.0%',
                t,
            ),
            sched(
                'SwitchPower interrupts: 10000 ms: 12950 SPIs from 13 INTIDs (1295/s): '
                '46 (spi 14) 11850 (1185/s), 131 (spi 99) 340 (34/s), 177 (spi 145) 100 (10/s)',
                t,
            ),
            sched(
                "SwitchSched system (10000 ms; user threads' kernel time by what entered the kernel, with entries): "
                'unix 10960 ms (239500), mach_msg 1620 ms (55280), other mach 440 ms (47680), '
                'data abort 2060 ms (125350), instruction abort 610 ms (8570), undefined 0 ms (0), '
                'other sync 0 ms (0), outside a trap 780 ms',
                t,
            ),
            sched(
                'SwitchSched callouts (10000 ms; on-core ms (calls), unslid): '
                'kernel 0xfffffff007dd3cd0 1790 ms (7350) (others 10 ms); '
                'timer actions 0xfffffff007ef8f70 110 ms (1100) (others 20 ms); unlisted 30 ms (400)',
                t,
            ),
        ]
        syms = _kernel_syms()
        busy = cpu.busy_windows(recs)
        irqs = cpu.irq_windows(recs)
        sys_ws = cpu.systime_windows(recs)
        call_ws = cpu.callout_windows(recs)
        self.assertEqual((busy[0].timer_irqs, busy[0].ipis, busy[0].spis), (15200, 60000, 11400))
        irq_text = '\n'.join(cpu.interrupt_summary(irqs, busy))
        self.assertIn('interrupts (1 windows): 1520 timer/s, 6000 ipi/s, 1140 spi/s', irq_text)
        self.assertIn('INTID 46 (SDMMC1) 1185/s', irq_text)
        self.assertIn('INTID 131 (PCIe WLAN MSI) 34/s', irq_text)
        self.assertIn('INTID 177 (SPI 145) 10/s', irq_text)
        sys_text = '\n'.join(cpu.systime_summary(sys_ws))
        self.assertIn('SwitchSched system (1647 ms/s over 1 windows): unix 1096 ms/s (23950/s)', sys_text)
        self.assertIn('data abort 206 ms/s (12535/s)', sys_text)
        self.assertIn('outside a trap 78 ms/s', sys_text)
        call_text = '\n'.join(cpu.callout_summary(call_ws, syms))
        self.assertIn('kernel _SwitchCryptex_finishRequest 179 ms/s (735/s)', call_text)
        self.assertIn('timer actions _timer_callout_hot 11 ms/s (110/s)', call_text)
        self.assertIn('unlisted 3 ms/s (40/s)', call_text)

    def test_kprof_parses_all_kernel_lines_and_pinned_windows_exclude_idle(self):
        t_pin = datetime(2026, 11, 26, 19, 38, 42, tzinfo=UTC)
        # An adjacent idle window grazing the pinned boundary by 5 ms must not bleed into pinned aggregates.
        t_adj_idle = datetime(2026, 11, 26, 19, 38, 51, 995000, tzinfo=UTC)
        t_idle = datetime(2026, 11, 26, 19, 51, 12, tzinfo=UTC)
        recs = [
            sched(
                'SwitchPower: 10000 ms: cpu0 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 0.0% busy 100.0%, '
                'cpu1 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 0.0% busy 100.0%, '
                'cpu2 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 0.0% busy 100.0%, '
                'cpu3 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 0.0% busy 100.0%; '
                'irq cpu0 3000 timer 15000 ipi 200 spi, cpu1 3000 timer 15000 ipi 9800 spi',
                t_pin,
            ),
            sched(
                'SwitchPower interrupts: 10000 ms: 10000 SPIs from 2 INTIDs (1000/s): 46 (spi 14) 9800 (980/s)',
                t_pin,
            ),
            sched(
                "SwitchSched cores (10000 ms; ms: a core's threads): "
                'cpu0 threads 9900 (irq 100), idle 100 = asleep 0 + irq 0 + rest 100, 100 sleeps; '
                'cpu1 threads 9900 (irq 100), idle 100 = asleep 0 + irq 0 + rest 100, 100 sleeps; '
                'cpu2 threads 9900 (irq 100), idle 100 = asleep 0 + irq 0 + rest 100, 100 sleeps; '
                'cpu3 threads 9900 (irq 100), idle 100 = asleep 0 + irq 0 + rest 100, 100 sleeps',
                t_pin,
            ),
            sched(
                'SwitchSched tasks (10000 ms; ms user+system, top 8 by both): '
                'SpringBoard[30] 9000+3000, kernel_task[0] 0+3000; '
                'all 200 processes 21000+18000, exited 0 (0+0), untracked 0',
                t_pin,
            ),
            sched(
                'SwitchKprof: 10000 ms, 4 cpus busy/irq %: 100/4.0 100/6.0 100/2.0 100/2.0; '
                'irq/ipi/exc per s: 1800/1500/3000 2200/1500/3000 1600/1500/2000 1600/1500/2000; '
                'samples 9600 (4 ms each): user 5000, kernel 4600 (kernel_task 900), lost 2; '
                'core MHz (probe): 1785 1785 1785 1785; governor MHz: 1785 1785 1785 1785',
                t_pin,
            ),
            sched(
                'SwitchKprof ipis per s, taken (ast/xcall/timer/wake/maint/other): '
                '900/100/50/200/250/0, 800/100/50/200/250/0; '
                'scheduler sent to running/idle CPUs: preempt 1600/400 rt 100/20',
                t_pin,
            ),
            sched(
                'SwitchKprof tasks (user+kernel samples=exact CPU): '
                'SpringBoard[30] 2000+1000=12000ms, kernel_task[0] 0+900=3000ms',
                t_pin,
            ),
            sched(
                'SwitchKprof kernel 1/2 (fn samples kernel_task <caller @owner): '
                'fffffff007123450 420 20 <fffffff007120000 @SpringBoard; '
                'filesystems.apfs@fffffff009000100 180 180 <? @kt:de;',
                t_pin,
            ),
            sched(
                'SwitchKprof kernel_task: 120 threads; ms in 10000 ms, name, top sampled fn: '
                "de '' 850 fffffff007a12340 ; 120 'thread call kernel #5' 1200 fffffff007dd3cd0",
                t_pin,
            ),
            sched(
                'SwitchKprof masked (samples kernel_task: masking code < its callers): '
                '65 15 fffffff007111110 <fffffff007222220;',
                t_pin,
            ),
            sched(
                'SwitchKprof faults: 8500/s (n, ms in vm_fault, longest us): '
                'zf 45000 1200 450, cow 10000 300 200, soft 15000 250 150, device 0 0 0, '
                'file 15000 4000 9000, cmp 0 0 0, failed 0 0 0; wired 100, kernel map 50, '
                'shared region 12000, copyio 500, purgeable zf 0; fast 9000/s (af 70000, mod 20000); xnu: faults 85000',
                t_pin,
            ),
            sched(
                'SwitchKprof faults by process 1/2 (n: zf cow soft device file cmp failed, ms; '
                'wired, shared region, copyio; fast af/mod): '
                'SpringBoard[30] 30000: 18000 2000 4000 0 6000 0 0, 1500 ms; 10, 5000, 100; 20000/5000;',
                t_pin,
            ),
            sched(
                'SwitchPower: 10000 ms: cpu0 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 80.0% busy 20.0%, '
                'cpu1 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 90.0% busy 10.0%; '
                'irq cpu0 100 timer 50 ipi 200 spi',
                t_adj_idle,
            ),
            sched(
                'SwitchPower interrupts: 10000 ms: 200 SPIs from 1 INTIDs (20/s): 122 (spi 90) 200 (20/s)',
                t_adj_idle,
            ),
            sched(
                'SwitchPower: 10000 ms: cpu0 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 80.0% busy 20.0%, '
                'cpu1 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 90.0% busy 10.0%; '
                'irq cpu0 100 timer 50 ipi 200 spi',
                t_idle,
            ),
            sched(
                'SwitchPower interrupts: 10000 ms: 200 SPIs from 1 INTIDs (20/s): 122 (spi 90) 200 (20/s)',
                t_idle,
            ),
            sched(
                'SwitchKprof: 10000 ms, 4 cpus busy/irq %: 20/1.0 10/0.5 0/0.0 0/0.0; '
                'samples 300 (4 ms each): user 200, kernel 100 (kernel_task 20), lost 0; core MHz (probe): 1020 1020',
                t_idle,
            ),
            sched(
                'SwitchKprof ipis per s, taken (ast/xcall/timer/wake/other): 10/0/5/5/0; '
                'scheduler sent to running/idle CPUs: preempt 10/5',
                t_idle,
            ),
            sched('SwitchKprof tasks (user+kernel samples=exact CPU): bluetoothd[96] 40+10=450ms', t_idle),
        ]
        syms = _kernel_syms()
        busy = cpu.busy_windows(recs)
        irqs = cpu.irq_windows(recs)
        tasks = cpu.tasks(recs)
        kps = cpu.kprof_windows(recs)
        self.assertEqual(len(kps), 2)

        whole = '\n'.join(cpu.kprof_summary(kps, syms))
        self.assertIn('core MHz probe min/p50/max 1020/1785/1785 (gov 1785..1785)', whole)
        self.assertIn('ast 855/s', whole)
        self.assertIn('maint 250/s', whole)
        self.assertIn('_vm_fault_enter 420s (kt 20s, <_vm_fault_internal, @SpringBoard)', whole)
        self.assertIn('filesystems.apfs@0xfffffff009000100 180s (kt 180s, @kt:de)', whole)
        self.assertIn('thread call kernel #5 120 ms/s (_SwitchCryptex_finishRequest)', whole)
        self.assertIn('kt:de 85 ms/s (_NativeSwitchBlock_worker)', whole)
        self.assertIn('_lck_spin_unlock < _thread_block_reason 65s (kt 15s)', whole)
        self.assertIn('kprof faults (8500/s): file 1500/s (400 ms/s), zf 4500/s (120 ms/s)', whole)
        self.assertIn('fast 9000/s (af 7000/s, mod 2000/s)', whole)
        self.assertIn('SpringBoard 3000/s (150 ms/s, zf 1800 file 600)', whole)

        pinned = '\n'.join(cpu.pinned_summary(cpu.CpuWindows(busy, tasks, kps, irqs), syms))
        self.assertIn('Pinned CPU (>= 3.5 busy cores): 1 of 3 windows (10 s), mean 4.00 busy cores', pinned)
        self.assertIn('600 timer/s, 3000 ipi/s, 1000 spi/s', pinned)
        self.assertIn('pinned SPI sources (1000/s over 1 windows): INTID 46 (SDMMC1) 980/s', pinned)
        self.assertNotIn('bluetoothd', pinned)
        self.assertNotIn('UART-D BT', pinned)
        self.assertIn('pinned kprof ipis taken: ast 1700/s, maint 500/s', pinned)


if __name__ == '__main__':
    unittest.main()
