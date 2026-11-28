/*
 * Switch kernel sampling profiler: where the CPU goes, kernel_task included, in the unified log every 10 s.
 * Debug kernels only, and off there unless built with --//kernel:kprof (SWITCH_KPROF). Release kernels keep only
 * switch_footprint_breakdown.
 *
 * Sampling. Each CPU takes one sample about every 4 ms from its own running timer (RUNNING_TIMER_SWITCH_KPROF).
 * Running timers are armed only while a non-idle thread runs, so an idle CPU is never woken for a sample and the cost
 * scales with busy time (one extra timer interrupt per 4 ms of work). A sample is the interrupted context
 * (cpu_int_state): a user-mode sample counts against its process; a kernel-mode sample also goes into a per-CPU table
 * keyed by function, from the kernel's LC_FUNCTION_STARTS (copied at boot, before __LINKEDIT is jettisoned), or by
 * 128-byte bucket outside the kernel image (Apple's prelinked kexts). Each function also keeps a majority-vote caller
 * and owner (the process, or the kernel_task thread). A user-mode sample goes into a per-CPU table keyed by process and
 * 64-byte bucket: the unslid dyld shared cache address, or the offset from the main executable's Mach-O header, or one
 * "other" bucket (app frameworks, JIT). It also goes into a small per-CPU table keyed by process and 1 MiB region of
 * the same key: a flat profile (SwiftUI, Objective-C) spreads over thousands of 64-byte buckets, so the eight busiest
 * cover a tenth of it, while its regions are a few dozen and name the images the time is in (r12-widgets: widget
 * extensions at 10-15 % coverage). A timer sample cannot see interrupt handlers or code that runs with interrupts
 * masked: those land where interrupts come back on, so the exact per-CPU interrupt time is logged too.
 *
 * Reporting. A thread call swaps every CPU's table with one cross-call, merges them and writes os_log lines (the log
 * only, never the console):
 *   Switch kprof: the window, per-CPU busy and interrupt time (exact, from recount), the sample split, and each
 *       core's clock as measured by a timed dependent-add chain (kprof_clock_probe)
 *   Switch kprof tasks: processes by samples, user+kernel
 *   Switch kprof kernel 1/2, 2/2: kernel functions by samples: unslid address (kext@address in a kext), samples in
 *       kernel_task threads, majority caller, majority owner
 *   Switch kprof kernel_task: kernel_task threads by exact CPU time, with name and their top sampled function
 *   Switch kprof user 1/2, 2/2: the busiest processes' user-mode hot spots: dsc <unslid address>, main+<offset>, other
 *   Switch kprof images: the same processes' user samples by 1 MiB region: dsc <unslid address >> 20>,
 *       main+ <offset >> 20>, other, then rest (regions past the logged ones) and lost (samples a full table refused)
 *   Switch kprof masked: where the samples that land on ml_set_interrupts_enabled come from. A timer sample taken
 *       while interrupts are masked is delivered at the unmask, so these samples are masked time; three return
 *       addresses name the code that masked (the scheduler's context switch, a driver's spinlock, ...)
 *   Switch kprof waits (r14-launch): where processes waited rather than ran, the busiest by wait plus CPU: exact CPU
 *       (recount, not samples), page-ins (count, KiB asked, ms the faulting threads waited; "cx" those from a cryptex,
 *       the dyld shared cache), cached-read misses (count, ms waited; kernel_task's are DiskImages reading the cryptex
 *       images), decmpfs page-ins, and "new +N ms" for a process started in the window (its age at the summary): the
 *       launch decomposition is wall = CPU + these waits + the rest (IPC, locks, scheduling)
 *   Switch kprof image (r14-launch, r17-launch): the cryptex images' reads on the direct read map (requests, KiB,
 *       card reads: SwitchCryptex.cpp), or, when DiskImages serves them, the cached reads of the image files, those
 *       that missed, the 64 KiB windows read ahead (vfs_cluster.c switch_image_window) and the card reads issued
 *   Switch kprof faults (r17-faults): every completed vm_fault of the window by class, with the time spent in
 *       vm_fault (entry to return, so a page-in's I/O wait is in it) and the longest: zf (zero fill), cow, soft (a
 *       resident page mapped again), device (an IOKit/device pager page: GPU pool views, IOSurfaces of physical
 *       memory), file (paged in), cmp (decompressed), failed; how many of them wired memory (vm_map_wire, IOKit
 *       prepare), were in a kernel map, in the task's shared region (the dyld shared cache), in a kernel copyin or
 *       copyout, and the zero fills of purgeable memory; the fast faults (pmap reference/modify emulation, which never
 *       reach vm_fault: access-flag and write-permission faults); and XNU's own vm_statistics64 counters over the
 *       window (vm_stat's), where zero fills outside vm_fault are UPL/IOKit prepare zero fills
 *   Switch kprof faults by process 1/2, 2/2: the processes with the most vm_faults, by class, with their fault time,
 *       wired/shared-region/copyio counts and fast faults
 *   Switch kprof zf sites: where the two processes with the most zero fills took them: the user PC (copyio: k, the
 *       syscall's) as dsc/main+ 64-byte bucket, with its majority LR
 *   The first line also carries each CPU's interrupts, IPIs and synchronous exceptions (faults and traps) per second.
 * User trap sites: a process's first brk (os_crash, __builtin_trap, Swift traps) prints one console line with the trap
 * site as main+<offset> or dsc <unslid address>, so a crash that takes the boot down (launchd _PanicOnCrash) still names
 * its os_crash call site.
 * Kernel addresses are unslid: `xcrun atos -o kernel.macho -arch arm64 <addr>...` with the build's kernel.macho names
 * them. dsc addresses: `ipsw dyld a2s <21H461 dyld_shared_cache_arm64> <addr>`; a region `dsc 18b9` is
 * 0x18b900000-0x18b9fffff (`ipsw dyld a2s -i <dsc> 0x18b980000` names its image; a region can straddle two).
 */

#include <kern/cpu_data.h>
#include <kern/kalloc.h>
#include <kern/ledger.h>
#include <kern/processor.h>
#include <kern/recount.h>
#include <kern/startup.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <kern/timer_call.h>
#include <libkern/OSKextLibPrivate.h>
#include <libkern/kernel_mach_header.h>
#include <mach-o/loader.h>
#include <machine/machine_routines.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include <vm/vm_shared_region.h>
#include <vm/vm_page.h>
#include <vm/vm_protos.h>
#include <kern/host_statistics.h>
#include <arm/cpu_data_internal.h>
#include <arm64/proc_reg.h>

/*
 * kern_memorystatus.c memorystatus_log_exception: what a process's phys_footprint is made of when it crosses a memory
 * limit (KiB), so a jetsam loop names its memory: anonymous (internal + compressed), IOKit mappings (GPU buffers and
 * IOSurfaces mapped into it), alternate accounting, page tables, purgeable, and the tagged/network/media/graphics
 * ledgers that are charged to the footprint.
 */
void switch_footprint_breakdown(task_t task, char *buf, size_t size);
void
switch_footprint_breakdown(task_t task, char *buf, size_t size)
{
	const struct {
		const char *name;
		int         entry;
	} parts[] = {
		{"footprint", task_ledgers.phys_footprint}, {"internal", task_ledgers.internal},
		{"compressed", task_ledgers.internal_compressed}, {"iokit_mapped", task_ledgers.iokit_mapped},
		{"alternate", task_ledgers.alternate_accounting}, {"alternate_compressed", task_ledgers.alternate_accounting_compressed},
		{"page_table", task_ledgers.page_table}, {"purgeable_nonvolatile", task_ledgers.purgeable_nonvolatile},
		{"tagged", task_ledgers.tagged_footprint}, {"network", task_ledgers.network_nonvolatile},
		{"media", task_ledgers.media_footprint}, {"graphics", task_ledgers.graphics_footprint},
		{"graphics_compressed", task_ledgers.graphics_footprint_compressed},
	};
	size_t len = 0;

	buf[0] = '\0';
	if (task == TASK_NULL || task->ledger == NULL) {
		return;
	}
	for (unsigned int i = 0; i < sizeof(parts) / sizeof(parts[0]) && len < size; i++) {
		ledger_amount_t balance = 0;
		int             n;

		if (parts[i].entry < 0 || ledger_get_balance(task->ledger, parts[i].entry, &balance) != KERN_SUCCESS) {
			continue;
		}
		n = snprintf(buf + len, size - len, "%s%s %lld", len ? ", " : "", parts[i].name, (long long)(balance >> 10));
		if (n > 0) {
			len += (size_t)n;
		}
	}
}

#if DEVELOPMENT || DEBUG

#define KPROF_TRAP_NAMES  32                /* processes whose first trap was printed */
#define KPROF_U_DSC       (1ull << 62)      /* user key: unslid dyld shared cache address */
#define KPROF_U_MAIN      (2ull << 62)      /* user key: offset from the main executable's Mach-O header */
#define KPROF_U_OTHER     (3ull << 62)      /* user key: anywhere else (app frameworks, JIT), one bucket */
#define KPROF_U_ADDR      ((1ull << 62) - 1)

/* A user PC as something the host can name: the unslid shared cache address, or main executable + offset. */
static uint64_t
kprof_user_location(task_t task, uint64_t pc)
{
	const struct vm_shared_region *sr = task->shared_region;
	uint64_t                       header = task->mach_header_vm_address;

	if (sr != NULL && pc >= sr->sr_base_address && pc - sr->sr_base_address < sr->sr_size) {
		return KPROF_U_DSC | (pc - sr->sr_slide);
	}
	if (header != 0 && pc >= header && pc - header < (256ull << 20)) {
		return KPROF_U_MAIN | (pc - header);
	}
	return KPROF_U_OTHER | (pc & KPROF_U_ADDR);
}

/* sleh.c handle_breakpoint, EL0: one console line for a process's first trap (os_crash and friends end in brk). */
void switch_kprof_user_trap(arm_saved_state_t *state, uint32_t esr);
void
switch_kprof_user_trap(arm_saved_state_t *state, uint32_t esr)
{
	static char         names[KPROF_TRAP_NAMES][32];
	static uint8_t      ready[KPROF_TRAP_NAMES];
	static unsigned int used;
	task_t              task = current_task();
	char                name[32];
	uint64_t            pc, lr;
	unsigned int        seen, slot;

	task_best_name(task, name, sizeof(name));
	seen = MIN(os_atomic_load(&used, acquire), KPROF_TRAP_NAMES);
	for (unsigned int i = 0; i < seen; i++) {
		if (os_atomic_load(&ready[i], acquire) && strncmp(names[i], name, sizeof(name)) == 0) {
			return;
		}
	}
	if (seen >= KPROF_TRAP_NAMES) {
		return;
	}
	slot = os_atomic_inc_orig(&used, relaxed);
	if (slot >= KPROF_TRAP_NAMES) {
		return;
	}
	strlcpy(names[slot], name, sizeof(names[slot]));
	os_atomic_store(&ready[slot], 1, release);
	pc = kprof_user_location(task, get_saved_state_pc(state));
	lr = kprof_user_location(task, get_saved_state_lr(state));
	printf("SwitchTrap: %s[%d] brk #0x%x at %s%llx, lr %s%llx\n", name, task_pid(task), ISS_BRK_COMMENT(esr),
	    (pc & ~KPROF_U_ADDR) == KPROF_U_DSC ? "dsc 0x" : (pc & ~KPROF_U_ADDR) == KPROF_U_MAIN ? "main+0x" : "0x",
	    pc & KPROF_U_ADDR,
	    (lr & ~KPROF_U_ADDR) == KPROF_U_DSC ? "dsc 0x" : (lr & ~KPROF_U_ADDR) == KPROF_U_MAIN ? "main+0x" : "0x",
	    lr & KPROF_U_ADDR);
}

#if defined(SWITCH_KPROF) && SWITCH_KPROF

extern void qsort(void *a, size_t n, size_t es, int (*cmp)(const void *, const void *));
extern int  vsnprintf(char *, size_t, const char *, va_list);

#define SWITCH_KPROF_ROLE_ON 1
#include <switch/kprof_role.h>

#define KPROF_PERIOD_NS   4000000ull        /* one sample per 4 ms of CPU work */
#define KPROF_WINDOW_NS   10000000000ull    /* one summary every 10 s */
#define KPROF_MAX_CPUS    8
#define KPROF_CPU_FNS     1024              /* per CPU and window: at most 2500 samples, most of them repeats */
#define KPROF_CPU_TASKS   128
#define KPROF_FNS         4096              /* merged */
#define KPROF_TASKS       256
#define KPROF_THREADS     1024              /* kernel_task threads tracked for exact time */
#define KPROF_PROBES      16
#define KPROF_TOP_FNS     20
#define KPROF_TOP_TASKS   14
#define KPROF_TOP_THREADS 8
#define KPROF_CPU_USER    2048              /* per CPU and window: (process, 64-byte bucket) pairs */
#define KPROF_USER        8192              /* merged */
#define KPROF_TOP_USER    4                 /* processes whose user hot spots are logged */
#define KPROF_TOP_USER_PC 8
#define KPROF_CPU_IMAGES  256               /* per CPU and window: (process, 1 MiB region) pairs */
#define KPROF_IMAGES      1024              /* merged */
#define KPROF_TOP_IMAGES  24                /* regions logged per process */
#define KPROF_REGION      (1ull << 20)
#define KPROF_CPU_MASKED  256               /* per CPU and window: unmask-point call chains */
#define KPROF_MASKED      1024              /* merged */
#define KPROF_TOP_MASKED  10
#define KPROF_LINE        900               /* under the 1 KiB os_log record */
#define KPROF_TID         (1ull << 63)      /* owner is a kernel_task thread id, not a pid */
#define KPROF_BUCKET      1ul               /* key is a 128-byte bucket, not a function start */
#define KPROF_U_COPYIO    (1ull << 61)      /* zf site key: a kernel copyin/copyout's fault, keyed by the syscall's PC */
#define KPROF_CPU_SITES   256               /* per CPU and window: (process, zero-fill PC bucket) pairs */
#define KPROF_SITES       1024              /* merged */
#define KPROF_TOP_SITES   6                 /* zf sites logged per process */
#define KPROF_TOP_FAULTS  8                 /* processes on the faults by process lines, four per line */

/* r17-faults: vm_fault classes and flags, as vm_fault.c classifies a fault for switch_kprof_fault (vm_protos.h) */
#define KPROF_F_ZF         SWITCH_FAULT_ZF
#define KPROF_F_COW        SWITCH_FAULT_COW
#define KPROF_F_SOFT       SWITCH_FAULT_SOFT
#define KPROF_F_DEVICE     SWITCH_FAULT_DEVICE
#define KPROF_F_FILE       SWITCH_FAULT_FILE
#define KPROF_F_CMP        SWITCH_FAULT_CMP
#define KPROF_F_FAILED     SWITCH_FAULT_FAILED
#define KPROF_F_CLASSES    SWITCH_FAULT_CLASSES

struct kprof_fn {
	uintptr_t key;          /* function start (slid) or 128-byte bucket | KPROF_BUCKET; 0: empty */
	uint64_t  caller;       /* majority-vote caller key */
	uint64_t  owner;        /* majority-vote owner: pid, or KPROF_TID | thread id */
	uint32_t  samples;
	uint32_t  kernel_task;  /* of which in kernel_task threads */
	uint32_t  caller_votes;
	uint32_t  owner_votes;
};

struct kprof_task {
	int32_t  pid;
	uint16_t user;
	uint16_t kernel;
#if SWITCH_KPROF_ROLE_ON
	uint16_t user_main;
#endif
	char     name[28];
	bool     used;
	bool     fresh;         /* merged table: the process started in this window (age_ms) */
	/* r14-launch, switch_kprof_wait: this window's waits (mach time) and counts */
	uint32_t pageins, pagein_kib, image_pageins, reads, read_kib, decmpfs, decmpfs_kib;
	uint64_t pagein_abs, image_abs, read_abs;
	uint64_t cpu_abs;       /* merged table: exact user+system CPU this window (kprof_refresh_names) */
	uint32_t age_ms;
	/* r17-faults, switch_kprof_fault/_fast_fault: this window's vm_faults by class, the time they took (mach time),
	 * how many wired memory, were in the shared region or in a kernel copyin/copyout, and the fast faults (access flag,
	 * write permission) */
	uint32_t faults[KPROF_F_CLASSES];
	uint32_t fault_wired, fault_dsc, fault_copyio, fast_af, fast_mod;
	uint64_t fault_abs;
};

/* r17-faults: the window's faults system-wide (every process, also those a full task table refused) */
struct kprof_faults {
	uint32_t n[KPROF_F_CLASSES];
	uint64_t abs[KPROF_F_CLASSES];      /* mach time in vm_fault */
	uint64_t max_abs[KPROF_F_CLASSES];
	uint64_t max_tid[KPROF_F_CLASSES];  /* the thread that took the longest, and its process */
	int32_t  max_pid[KPROF_F_CLASSES];
	uint32_t wired, kernel, dsc, copyio, purgeable, fast_af, fast_mod;
};

/* r17-faults: a zero fill's user PC (64-byte bucket, KPROF_U_* key, KPROF_U_COPYIO for a kernel copyio's fault) with
 * its majority LR, for the zf sites line */
struct kprof_site {
	uint64_t key;           /* 0: empty */
	uint64_t lr;            /* majority-vote LR (kprof_user_location form) */
	int32_t  pid;
	uint32_t n;
	uint32_t lr_votes;
};

/* r14-launch: each live process's lifetime CPU at the last summary, by unique id, for exact per-window CPU. */
struct kprof_cpu_last {
	uint64_t  uniqueid;     /* 0: empty */
	uintptr_t task;         /* an exec in place keeps the proc's unique id but makes a new task */
	uint64_t  cpu_abs;
};
#define KPROF_CPU_LAST    2048

#if SWITCH_KPROF_ROLE_ON
typedef struct switch_kprof_user kprof_user_t;
#else
struct kprof_user {
	uint64_t key;           /* KPROF_U_* | bucket; 0: empty */
	int32_t  pid;
	uint32_t samples;
};
typedef struct kprof_user kprof_user_t;
#endif

struct kprof_masked {
	uintptr_t chain[3];     /* the masking code's return addresses as function keys; chain[0] 0: empty */
	uint32_t  samples;
	uint32_t  kernel_task;
};

struct kprof_buf {
	uint32_t            samples, user, kernel, kernel_task, lost;
	struct kprof_fn     fn[KPROF_CPU_FNS];
	struct kprof_task   task[KPROF_CPU_TASKS];
	kprof_user_t        user_pc[KPROF_CPU_USER];
	kprof_user_t        image[KPROF_CPU_IMAGES];  /* key: kprof_region_key() */
	uint32_t            image_lost;
	struct kprof_masked masked[KPROF_CPU_MASKED];
	struct kprof_faults faults;
	struct kprof_site   site[KPROF_CPU_SITES];
};

struct kprof_cpu {
	struct kprof_buf *buf[2];
	unsigned int      active;   /* the buffer the sampler fills; the summary's cross-call flips it */
	bool              armed;
	uint64_t          deadline;
	uint32_t          rng;
	uint64_t          last_idle, last_irq;
	uint64_t          last_irqs, last_ipis, last_exc;  /* cpu_stat counts at the last summary */
	bool              primed;     /* last_idle/last_irq hold a previous summary's values */
};

/* drivers/SwitchPower/SwitchPerfGovernor.cpp: the clock the on-demand CPU governor last applied, in kHz (0: none). */
extern unsigned int switch_perf_cpu_khz(void);

struct kprof_thread {
	uint64_t tid;
	uint64_t time;
};

static struct kprof_cpu    kprof_cpus[KPROF_MAX_CPUS];
static unsigned int        kprof_ncpus;
static bool                kprof_ready;
static uint64_t            kprof_period, kprof_window;
static uintptr_t           kprof_image, kprof_text_start, kprof_text_end;
static uint32_t           *kprof_starts;        /* function starts, offsets from kprof_image, ascending */
static uint32_t            kprof_nstarts;
static struct kprof_fn    *kprof_fns;           /* merged window */
static struct kprof_task  *kprof_tasks;
static kprof_user_t       *kprof_users;
static kprof_user_t       *kprof_images;        /* merged (process, region) table */
static uint32_t            kprof_images_lost;
static struct kprof_masked *kprof_masks;        /* merged window */
static struct kprof_faults kprof_fault_sum;     /* merged window (r17-faults) */
static struct kprof_site  *kprof_sites;         /* merged window (r17-faults) */
static uintptr_t           kprof_unmask[2];     /* ml_set_interrupts_enabled(_with_debug) as function keys */
static struct kprof_thread *kprof_threads[2];   /* kernel_task threads: this window, last window (sorted by tid) */
static uint64_t           *kprof_vals;          /* top-N scratch, KPROF_USER entries */
static struct kprof_cpu_last *kprof_cpu_last[2]; /* this summary, the last (r14-launch) */
static unsigned int        kprof_cpu_last_cur;
static bool                kprof_cpu_last_primed;
static uint32_t            kprof_nthreads[2];
static unsigned int        kprof_thread_cur;
static uint64_t            kprof_last_summary;
static thread_call_t       kprof_call;
static char                kprof_line[KPROF_LINE];
static size_t              kprof_len;

/* --- sampling (timer interrupt context, interrupts off) ------------------------------------------------------------ */

static uintptr_t
kprof_fn_key(uintptr_t pc)
{
	if (pc >= kprof_text_start && pc < kprof_text_end && kprof_nstarts != 0) {
		uint32_t off = (uint32_t)(pc - kprof_image);
		uint32_t lo = 0, hi = kprof_nstarts;

		if (kprof_starts[0] <= off) {
			while (hi - lo > 1) {
				uint32_t mid = lo + (hi - lo) / 2;
				if (kprof_starts[mid] <= off) {
					lo = mid;
				} else {
					hi = mid;
				}
			}
			return kprof_image + kprof_starts[lo];
		}
	}
	return (pc & ~(uintptr_t)127) | KPROF_BUCKET;
}

static inline uint32_t
kprof_hash(uint64_t key, unsigned int bits)
{
	return (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> (64 - bits));
}

static void
kprof_vote(uint64_t *cand, uint32_t *votes, uint64_t value)
{
	if (*cand == value) {
		(*votes)++;
	} else if (*votes == 0) {
		*cand = value;
		*votes = 1;
	} else {
		(*votes)--;
	}
}

static void
kprof_vote_merge(uint64_t *cand, uint32_t *votes, uint64_t other, uint32_t other_votes)
{
	if (*cand == other) {
		*votes += other_votes;
	} else if (other_votes > *votes) {
		*cand = other;
		*votes = other_votes - *votes;
	} else {
		*votes -= other_votes;
	}
}

static struct kprof_fn *
kprof_fn_slot(struct kprof_fn *table, unsigned int bits, uintptr_t key)
{
	uint32_t mask = (1u << bits) - 1;
	uint32_t i = kprof_hash(key, bits);

	for (unsigned int probe = 0; probe < KPROF_PROBES; probe++, i = (i + 1) & mask) {
		if (table[i].key == key) {
			return &table[i];
		}
		if (table[i].key == 0) {
			table[i].key = key;
			return &table[i];
		}
	}
	return NULL;
}

static struct kprof_task *
kprof_task_slot(struct kprof_task *table, unsigned int bits, int32_t pid)
{
	uint32_t mask = (1u << bits) - 1;
	uint32_t i = kprof_hash((uint64_t)(uint32_t)pid + 1, bits);

	for (unsigned int probe = 0; probe < KPROF_PROBES; probe++, i = (i + 1) & mask) {
		if (table[i].used && table[i].pid == pid) {
			return &table[i];
		}
		if (!table[i].used) {
			table[i].used = true;
			table[i].pid = pid;
			return &table[i];
		}
	}
	return NULL;
}

#if !SWITCH_KPROF_ROLE_ON
static struct kprof_user *
kprof_user_slot(struct kprof_user *table, unsigned int bits, int32_t pid, uint64_t key)
{
	uint32_t mask = (1u << bits) - 1;
	uint32_t i = kprof_hash(key ^ ((uint64_t)(uint32_t)pid << 32), bits);

	for (unsigned int probe = 0; probe < KPROF_PROBES; probe++, i = (i + 1) & mask) {
		if (table[i].key == key && table[i].pid == pid) {
			return &table[i];
		}
		if (table[i].key == 0) {
			table[i].key = key;
			table[i].pid = pid;
			return &table[i];
		}
	}
	return NULL;
}
#endif

#if !SWITCH_KPROF_ROLE_ON
static uint64_t
kprof_user_key(task_t task, uint64_t pc)
{
	uint64_t where = kprof_user_location(task, pc);

	return (where & ~KPROF_U_ADDR) == KPROF_U_OTHER ? KPROF_U_OTHER : (where & ~63ull);
}

/* The 1 MiB region of a user key, for the images line: dyld shared cache images are contiguous, so a region names one
 * image (or two at a boundary), and a flat profile collapses into a few dozen of them. */
static uint64_t
kprof_region_key(uint64_t key)
{
	return (key & ~KPROF_U_ADDR) | ((key & KPROF_U_ADDR) & ~(KPROF_REGION - 1));
}
#endif

/* The caller of the sampled function: LR in a leaf, else the return address in the function's frame record. */
static uintptr_t
kprof_caller(arm_saved_state_t *state, thread_t thread, uintptr_t fn)
{
	uintptr_t caller = kprof_fn_key(get_saved_state_lr(state));
	uintptr_t fp, stack = thread->kernel_stack;

	if (caller != fn) {
		return caller;
	}
	fp = get_saved_state_fp(state);
	if (stack == 0 || (fp & 7) != 0 || fp < stack || fp > stack + kernel_stack_size - 16) {
		return 0;
	}
	return kprof_fn_key(((const uintptr_t *)fp)[1]);
}

/* Up to three return addresses above the sampled function, as function keys: LR when it is a leaf, then frame records. */
static void
kprof_chain(arm_saved_state_t *state, thread_t thread, uintptr_t fn, uintptr_t chain[3])
{
	uintptr_t    fp = get_saved_state_fp(state), stack = thread->kernel_stack;
	unsigned int n = 0;

	chain[0] = chain[1] = chain[2] = 0;
	if (kprof_fn_key(get_saved_state_lr(state)) != fn) {
		chain[n++] = kprof_fn_key(get_saved_state_lr(state));
	}
	while (n < 3 && stack != 0 && (fp & 7) == 0 && fp >= stack && fp <= stack + kernel_stack_size - 16) {
		const uintptr_t *frame = (const uintptr_t *)fp;

		chain[n++] = kprof_fn_key(frame[1]);
		if (frame[0] <= fp) {
			break;
		}
		fp = frame[0];
	}
}

static struct kprof_masked *
kprof_masked_slot(struct kprof_masked *table, unsigned int bits, const uintptr_t chain[3])
{
	uint32_t mask = (1u << bits) - 1;
	uint32_t i = kprof_hash(chain[0] ^ (chain[1] << 1) ^ (chain[2] << 2), bits);

	for (unsigned int probe = 0; probe < KPROF_PROBES; probe++, i = (i + 1) & mask) {
		if (table[i].chain[0] == chain[0] && table[i].chain[1] == chain[1] && table[i].chain[2] == chain[2]) {
			return &table[i];
		}
		if (table[i].chain[0] == 0) {
			memcpy(table[i].chain, chain, sizeof(table[i].chain));
			return &table[i];
		}
	}
	return NULL;
}

static struct kprof_site *
kprof_site_slot(struct kprof_site *table, unsigned int bits, int32_t pid, uint64_t key)
{
	uint32_t mask = (1u << bits) - 1;
	uint32_t i = kprof_hash(key ^ ((uint64_t)(uint32_t)pid << 32), bits);

	for (unsigned int probe = 0; probe < KPROF_PROBES; probe++, i = (i + 1) & mask) {
		if (table[i].key == key && table[i].pid == pid) {
			return &table[i];
		}
		if (table[i].key == 0) {
			table[i].key = key;
			table[i].pid = pid;
			return &table[i];
		}
	}
	return NULL;
}

static void
kprof_sample(struct kprof_cpu *kc)
{
	arm_saved_state_t *state = getCpuDatap()->cpu_int_state;
	struct kprof_buf  *b = kc->buf[kc->active];
	thread_t           thread = current_thread();
	task_t             task = get_threadtask(thread);
	struct kprof_task *ts;
	struct kprof_fn   *f;
	uintptr_t          fn;
	bool               kt;

	if (state == NULL || task == TASK_NULL) {
		return;
	}
	b->samples++;
	ts = kprof_task_slot(b->task, 7, task_pid(task));
	if (ts != NULL && ts->name[0] == '\0') {
		task_best_name(task, ts->name, sizeof(ts->name));
	}
	if (PSR64_IS_USER(get_saved_state_cpsr(state))) {
#if SWITCH_KPROF_ROLE_ON
		const bool     is_main = (thread->thread_tag & THREAD_TAG_MAINTHREAD) != 0;
		const uint64_t key = switch_kprof_user_key(kprof_user_location(task, get_saved_state_pc(state)), is_main);
		kprof_user_t  *u = switch_kprof_user_slot(b->user_pc, 11, task_pid(task), key);
		kprof_user_t  *r = switch_kprof_user_slot(b->image, 8, task_pid(task), switch_kprof_user_region_key(key));

		b->user++;
		if (ts != NULL) {
			ts->user++;
			if (is_main) {
				ts->user_main++;
			}
		}
#else
		const uint64_t     key = kprof_user_key(task, get_saved_state_pc(state));
		struct kprof_user *u = kprof_user_slot(b->user_pc, 11, task_pid(task), key);
		struct kprof_user *r = kprof_user_slot(b->image, 8, task_pid(task), kprof_region_key(key));

		b->user++;
		if (ts != NULL) {
			ts->user++;
		}
#endif
		if (u != NULL) {
			u->samples++;
		}
		if (r != NULL) {
			r->samples++;
		} else {
			b->image_lost++;
		}
		return;
	}
	b->kernel++;
	if (ts != NULL) {
		ts->kernel++;
	}
	kt = (task == kernel_task);
	if (kt) {
		b->kernel_task++;
	}
	fn = kprof_fn_key(get_saved_state_pc(state));
	f = kprof_fn_slot(b->fn, 10, fn);
	if (f == NULL) {
		b->lost++;
		return;
	}
	f->samples++;
	if (kt) {
		f->kernel_task++;
	}
	kprof_vote(&f->caller, &f->caller_votes, kprof_caller(state, thread, fn));
	kprof_vote(&f->owner, &f->owner_votes, kt ? (KPROF_TID | thread_tid(thread)) : (uint64_t)(uint32_t)task_pid(task));
	if (fn == kprof_unmask[0] || fn == kprof_unmask[1]) {
		uintptr_t           chain[3];
		struct kprof_masked *m;

		kprof_chain(state, thread, fn, chain);
		if (chain[0] != 0 && (m = kprof_masked_slot(b->masked, 8, chain)) != NULL) {
			m->samples++;
			if (kt) {
				m->kernel_task++;
			}
		}
	}
}

static uint64_t
kprof_next(struct kprof_cpu *kc, uint64_t from)
{
	/* +-1/8 of a period of jitter, so a sample never locks onto a periodic kernel activity */
	kc->rng ^= kc->rng << 13;
	kc->rng ^= kc->rng >> 17;
	kc->rng ^= kc->rng << 5;
	return from + kprof_period - kprof_period / 8 + (kc->rng % (kprof_period / 4));
}

/* The next point of this CPU's sampling grid after now: busy bursts of any length are sampled in proportion. */
static uint64_t
kprof_catch_up(struct kprof_cpu *kc, uint64_t now)
{
	uint64_t deadline = kc->deadline;

	if (deadline <= now) {
		deadline = now + kprof_period - (now - deadline) % kprof_period;
	}
	return deadline;
}

void
switch_kprof_expire(void *param0, __unused void *param1)
{
	processor_t       processor = param0;
	struct kprof_cpu *kc = &kprof_cpus[processor->cpu_id];
	uint64_t          now = mach_absolute_time();

	if (!kprof_ready || (unsigned int)processor->cpu_id >= kprof_ncpus) {
		running_timer_clear(processor, RUNNING_TIMER_SWITCH_KPROF);
		return;
	}
	kprof_sample(kc);
	kc->deadline = kprof_next(kc, kc->deadline);
	kc->deadline = kprof_catch_up(kc, now);
	running_timer_enter(processor, RUNNING_TIMER_SWITCH_KPROF, NULL, kc->deadline, now);
}

/* thread_dispatch, leaving idle: re-arm on this CPU's grid (the deadline went stale while idle). */
void
switch_kprof_running_setup(processor_t processor, uint64_t now)
{
	struct kprof_cpu *kc = &kprof_cpus[processor->cpu_id];

	if (!kprof_ready || (unsigned int)processor->cpu_id >= kprof_ncpus) {
		return;
	}
	kc->armed = true;
	kc->deadline = kprof_catch_up(kc, now);
	running_timer_setup(processor, RUNNING_TIMER_SWITCH_KPROF, NULL, kc->deadline, now);
}

/*
 * r14-launch: a wait (or a count) for the current process, into this CPU's active table like a sample (interrupts off,
 * so the summary's cross-call cannot flip the table under it). Called by the vnode pager around a fault's page-in
 * (kind 0, or 3 for a cryptex file: the dyld shared cache), by cluster_read_copy for a cached read that missed (1), and by
 * decmpfs for a compressed page-in (2, bytes only). Never on an interrupt path; costs one table probe per page-in.
 */
void switch_kprof_wait(unsigned int kind, uint64_t elapsed, uint64_t bytes);
void
switch_kprof_wait(unsigned int kind, uint64_t elapsed, uint64_t bytes)
{
	task_t             task = current_task();
	struct kprof_task *ts;
	boolean_t          istate;
	unsigned int       cpu;
	uint32_t           kib = (uint32_t)MIN(bytes >> 10, UINT32_MAX);

	if (!os_atomic_load(&kprof_ready, acquire) || task == TASK_NULL) {
		return;
	}
	istate = ml_set_interrupts_enabled(FALSE);
	cpu = (unsigned int)cpu_number();
	if (cpu < kprof_ncpus && (ts = kprof_task_slot(kprof_cpus[cpu].buf[kprof_cpus[cpu].active]->task, 7,
	    task_pid(task))) != NULL) {
		if (ts->name[0] == '\0') {
			task_best_name(task, ts->name, sizeof(ts->name));
		}
		switch (kind) {
		case 0:
		case 3:
			ts->pageins++;
			ts->pagein_kib += kib;
			ts->pagein_abs += elapsed;
			if (kind == 3) {
				ts->image_pageins++;
				ts->image_abs += elapsed;
			}
			break;
		case 1:
			ts->reads++;
			ts->read_kib += kib;
			ts->read_abs += elapsed;
			break;
		default:
			ts->decmpfs++;
			ts->decmpfs_kib += kib;
			break;
		}
	}
	ml_set_interrupts_enabled(istate);
}

/*
 * r17-faults: one completed vm_fault (vm_fault.c vm_fault_internal, as it returns) for the current process, into this
 * CPU's active table like a sample (interrupts off, so the summary's cross-call cannot flip the table under it): its
 * class, flags and time in vm_fault, the shared-region and copyio tests, and for a zero fill its user PC and LR from
 * the thread's user state (the faulting instruction, or for a kernel copyin/copyout the syscall that made it). Two
 * table probes per fault, never on an interrupt path (vm_fault refuses those).
 */
void
switch_kprof_fault(unsigned int cls, unsigned int flags, uint64_t began, uint64_t vaddr)
{
	const uint64_t       now = mach_absolute_time();
	thread_t             thread = current_thread();
	task_t               task = current_task();
	struct kprof_buf    *b;
	struct kprof_task   *ts;
	boolean_t            istate;
	unsigned int         cpu;
	uint64_t             took = now - MIN(now, began);
	bool                 copyio = false, dsc = false;
	arm_saved_state_t   *upcb = NULL;
	uint64_t             pc = 0, lr = 0;

	if (!os_atomic_load(&kprof_ready, acquire) || task == TASK_NULL || cls >= KPROF_F_CLASSES) {
		return;
	}
	if (task != kernel_task && !(flags & SWITCH_FAULT_KERNEL) && (upcb = thread->machine.upcb) != NULL) {
		const struct vm_shared_region *sr = task->shared_region;
		const uint32_t                 ec = ESR_EC(get_saved_state_esr(upcb));

		dsc = sr != NULL && vaddr >= sr->sr_base_address && vaddr - sr->sr_base_address < sr->sr_size;
		/*
		 * The user state holds the last exception from EL0: this fault's own abort, or else the syscall (or trap)
		 * the kernel is serving, whose copyin/copyout (or vm_map_copyout, IOKit wiring) touched the user map.
		 */
		copyio = !(flags & SWITCH_FAULT_WIRE) && ec != ESR_EC_DABORT_EL0 && ec != ESR_EC_IABORT_EL0;
		if (cls == KPROF_F_ZF) {
			pc = get_saved_state_pc(upcb);
			lr = get_saved_state_lr(upcb);
		} else {
			upcb = NULL;
		}
	}
	istate = ml_set_interrupts_enabled(FALSE);
	cpu = (unsigned int)cpu_number();
	if (cpu >= kprof_ncpus) {
		ml_set_interrupts_enabled(istate);
		return;
	}
	b = kprof_cpus[cpu].buf[kprof_cpus[cpu].active];
	b->faults.n[cls]++;
	b->faults.abs[cls] += took;
	if (took > b->faults.max_abs[cls]) {
		b->faults.max_abs[cls] = took;
		b->faults.max_tid[cls] = thread_tid(thread);
		b->faults.max_pid[cls] = task_pid(task);
	}
	b->faults.wired += (flags & SWITCH_FAULT_WIRE) != 0;
	b->faults.kernel += (flags & SWITCH_FAULT_KERNEL) != 0;
	b->faults.purgeable += (flags & SWITCH_FAULT_PURGEABLE) != 0;
	b->faults.dsc += dsc;
	b->faults.copyio += copyio;
	ts = kprof_task_slot(b->task, 7, task_pid(task));
	if (ts != NULL) {
		if (ts->name[0] == '\0') {
			task_best_name(task, ts->name, sizeof(ts->name));
		}
		ts->faults[cls]++;
		ts->fault_abs += took;
		ts->fault_wired += (flags & SWITCH_FAULT_WIRE) != 0;
		ts->fault_dsc += dsc;
		ts->fault_copyio += copyio;
	}
	if (upcb != NULL) {
		const uint64_t     where = kprof_user_location(task, pc);
		const uint64_t     key = ((where & ~KPROF_U_ADDR) == KPROF_U_OTHER ? KPROF_U_OTHER : (where & ~63ull)) |
		    (copyio ? KPROF_U_COPYIO : 0);
		struct kprof_site *s = kprof_site_slot(b->site, 8, task_pid(task), key);

		if (s != NULL) {
			s->n++;
			kprof_vote(&s->lr, &s->lr_votes, kprof_user_location(task, lr));
		}
	}
	ml_set_interrupts_enabled(istate);
}

/* r17-faults: sleh.c, a user or kernel abort arm_fast_fault resolved (pmap reference/modify emulation). */
void
switch_kprof_fast_fault(bool access_flag)
{
	task_t             task = current_task();
	struct kprof_buf  *b;
	struct kprof_task *ts;
	boolean_t          istate;
	unsigned int       cpu;

	if (!os_atomic_load(&kprof_ready, acquire) || task == TASK_NULL) {
		return;
	}
	istate = ml_set_interrupts_enabled(FALSE);
	cpu = (unsigned int)cpu_number();
	if (cpu < kprof_ncpus) {
		b = kprof_cpus[cpu].buf[kprof_cpus[cpu].active];
		if (access_flag) {
			b->faults.fast_af++;
		} else {
			b->faults.fast_mod++;
		}
		if ((ts = kprof_task_slot(b->task, 7, task_pid(task))) != NULL) {
			if (ts->name[0] == '\0') {
				task_best_name(task, ts->name, sizeof(ts->name));
			}
			if (access_flag) {
				ts->fast_af++;
			} else {
				ts->fast_mod++;
			}
		}
	}
	ml_set_interrupts_enabled(istate);
}

/* --- summary (thread call) ---------------------------------------------------------------------------------------- */

/* Cross-call on every CPU: the sampler moves to the other buffer; arm CPUs that have not left idle since boot. */
static void
kprof_flip_cpu(__unused void *arg)
{
	processor_t       processor = current_processor();
	struct kprof_cpu *kc = &kprof_cpus[processor->cpu_id];
	uint64_t          now = mach_absolute_time();

	if ((unsigned int)processor->cpu_id >= kprof_ncpus) {
		return;
	}
	kc->active ^= 1;
	if (!kc->armed) {
		kc->armed = true;
		kc->deadline = now + kprof_period;
		running_timer_enter(processor, RUNNING_TIMER_SWITCH_KPROF, NULL, kc->deadline, now);
	}
}

static void
kprof_put(const char *fmt, ...) __printflike(1, 2);

static void
kprof_put(const char *fmt, ...)
{
	va_list args;
	int     n;

	if (kprof_len >= sizeof(kprof_line) - 1) {
		return;
	}
	va_start(args, fmt);
	n = vsnprintf(kprof_line + kprof_len, sizeof(kprof_line) - kprof_len, fmt, args);
	va_end(args);
	if (n > 0) {
		kprof_len = MIN(kprof_len + (size_t)n, sizeof(kprof_line) - 1);
	}
}

static void
kprof_flush(void)
{
	os_log(OS_LOG_DEFAULT, "%s", kprof_line);
	kprof_len = 0;
	kprof_line[0] = '\0';
}

static const char *
kprof_task_name(uint64_t owner, char *buf, size_t size)
{
	if (owner & KPROF_TID) {
		snprintf(buf, size, "kt:%llx", owner & ~KPROF_TID);
		return buf;
	}
	for (unsigned int i = 0; i < KPROF_TASKS; i++) {
		if (kprof_tasks[i].used && (uint64_t)(uint32_t)kprof_tasks[i].pid == owner) {
			return kprof_tasks[i].name;
		}
	}
	snprintf(buf, size, "pid %llu", owner);
	return buf;
}

/* An address as the build's kernel.macho has it (the prelinked kexts are in it too), with the kext's name outside the
 * kernel image. */
static void
kprof_put_addr(const char *prefix, uintptr_t key)
{
	OSKextLoadedKextSummary kext;
	uintptr_t               addr = key & ~KPROF_BUCKET;
	const char             *name;

	if (key == 0) {
		kprof_put("%s?", prefix);
		return;
	}
	if ((addr < kprof_text_start || addr >= kprof_text_end) &&
	    OSKextGetLoadedKextSummaryForAddress((const void *)addr, &kext) == KERN_SUCCESS) {
		name = kext.name;
		if (strncmp(name, "com.apple.", 10) == 0) {
			name += 10;
		}
		kprof_put("%s%s@%lx", prefix, name, (unsigned long)VM_KERNEL_UNSLIDE(addr));
		return;
	}
	kprof_put("%s%lx", prefix, (unsigned long)VM_KERNEL_UNSLIDE(addr));
}

/* Indices of the n largest non-zero values, largest first. */
static unsigned int
kprof_top(const uint64_t *values, unsigned int count, unsigned int *top, unsigned int n)
{
	unsigned int have = 0;

	for (unsigned int i = 0; i < count; i++) {
		uint64_t     v = values[i];
		unsigned int at;

		if (v == 0 || (have == n && v <= values[top[n - 1]])) {
			continue;
		}
		at = (have < n) ? have++ : n - 1;
		while (at > 0 && values[top[at - 1]] < v) {
			top[at] = top[at - 1];
			at--;
		}
		top[at] = i;
	}
	return have;
}

static int
kprof_thread_cmp(const void *a, const void *b)
{
	uint64_t x = ((const struct kprof_thread *)a)->tid, y = ((const struct kprof_thread *)b)->tid;

	return (x > y) - (x < y);
}

static uint64_t
kprof_thread_prev(uint64_t tid)
{
	const struct kprof_thread *prev = kprof_threads[kprof_thread_cur ^ 1];
	uint32_t lo = 0, hi = kprof_nthreads[kprof_thread_cur ^ 1];

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (prev[mid].tid == tid) {
			return prev[mid].time;
		}
		if (prev[mid].tid < tid) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return 0;
}

static void
kprof_kernel_task_threads(uint64_t window)
{
	struct kprof_thread *cur = kprof_threads[kprof_thread_cur];
	unsigned int         top[KPROF_TOP_THREADS], ntop;
	uint64_t             delta[KPROF_TOP_THREADS];
	char                 names[KPROF_TOP_THREADS][MAXTHREADNAMESIZE];
	uint32_t             n = 0;
	thread_t             thread;

	task_lock(kernel_task);
	queue_iterate(&kernel_task->threads, thread, thread_t, task_threads) {
		struct recount_usage usage = {};

		if (n == KPROF_THREADS) {
			break;
		}
		if (thread->options & TH_OPT_IDLE_THREAD) {
			continue;
		}
		recount_thread_usage(thread, &usage);
		cur[n].tid = thread_tid(thread);
		cur[n].time = recount_usage_time_mach(&usage);
		n++;
	}
	task_unlock(kernel_task);
	qsort(cur, n, sizeof(cur[0]), kprof_thread_cmp);
	kprof_nthreads[kprof_thread_cur] = n;

	for (uint32_t i = 0; i < n; i++) {
		kprof_vals[i] = cur[i].time - MIN(cur[i].time, kprof_thread_prev(cur[i].tid));
	}
	ntop = kprof_top(kprof_vals, n, top, KPROF_TOP_THREADS);
	for (unsigned int i = 0; i < ntop; i++) {
		delta[i] = kprof_vals[top[i]];
		names[i][0] = '\0';
	}
	task_lock(kernel_task);
	queue_iterate(&kernel_task->threads, thread, thread_t, task_threads) {
		for (unsigned int i = 0; i < ntop; i++) {
			if (cur[top[i]].tid == thread_tid(thread)) {
				thread_get_thread_name(thread, names[i]);
			}
		}
	}
	task_unlock(kernel_task);

	kprof_put("SwitchKprof kernel_task: %u threads; ms in %llu ms, name, top sampled fn:", n,
	    window / (NSEC_PER_MSEC));
	for (unsigned int i = 0; i < ntop; i++) {
		uint64_t ns, best_samples = 0;
		uintptr_t best = 0;

		absolutetime_to_nanoseconds(delta[i], &ns);
		for (unsigned int j = 0; j < KPROF_FNS; j++) {
			if (kprof_fns[j].key != 0 && kprof_fns[j].owner == (KPROF_TID | cur[top[i]].tid) &&
			    kprof_fns[j].samples > best_samples) {
				best_samples = kprof_fns[j].samples;
				best = kprof_fns[j].key;
			}
		}
		kprof_put(" %s%llx '%s' %llu", i ? "; " : "", cur[top[i]].tid, names[i], ns / NSEC_PER_MSEC);
		if (best != 0) {
			kprof_put_addr(" ", best);
		}
	}
	kprof_flush();
	kprof_thread_cur ^= 1;
}

/* bsd/kern/decmpfs.c: compressed page-ins of chunked files, their bytes and the 64 KiB chunks those bytes touched;
 * osfmk/vm/vm_object.c: the page-ins grown to whole chunks and the pages that added (r14-launch). */
extern uint64_t switch_decmpfs_pageins, switch_decmpfs_small, switch_decmpfs_bytes, switch_decmpfs_chunk_bytes;
extern uint64_t switch_decmpfs_aligned, switch_decmpfs_aligned_pages;

static void
kprof_decmpfs_line(void)
{
	static uint64_t last[6];
	uint64_t        now[6] = {
		os_atomic_load(&switch_decmpfs_pageins, relaxed), os_atomic_load(&switch_decmpfs_small, relaxed),
		os_atomic_load(&switch_decmpfs_bytes, relaxed), os_atomic_load(&switch_decmpfs_chunk_bytes, relaxed),
		os_atomic_load(&switch_decmpfs_aligned, relaxed), os_atomic_load(&switch_decmpfs_aligned_pages, relaxed),
	};
	uint64_t        d[6];

	for (unsigned int i = 0; i < 6; i++) {
		d[i] = now[i] - last[i];
		last[i] = now[i];
	}
	if (d[0] == 0) {
		return;
	}
	kprof_put("SwitchKprof decmpfs: %llu chunked page-ins (%llu of one page), %llu KiB paged in from %llu KiB of "
	    "64 KiB chunks (%llu.%llux); %llu grown to whole chunks (+%llu KiB)", d[0], d[1], d[2] >> 10, d[3] >> 10,
	    d[3] / MAX(d[2], 1), d[3] * 10 / MAX(d[2], 1) % 10, d[4], d[5] * (PAGE_SIZE >> 10));
	kprof_flush();
}

/* The busiest processes' user-mode hot spots, two processes per line. */
#if SWITCH_KPROF_ROLE_ON
static void
kprof_user_hot_spots(void)
{
	static char  proc_line[KPROF_LINE];
	unsigned int procs[KPROF_TOP_USER], nprocs;

	for (unsigned int i = 0; i < KPROF_TASKS; i++) {
		kprof_vals[i] = kprof_tasks[i].used ? kprof_tasks[i].user : 0;
	}
	nprocs = kprof_top(kprof_vals, KPROF_TASKS, procs, KPROF_TOP_USER);
	for (unsigned int p = 0; p < nprocs; p++) {
		const struct kprof_task *t = &kprof_tasks[procs[p]];

		switch_kprof_format_user_proc(proc_line, sizeof(proc_line), t->name, t->pid, t->user, t->user_main,
		    kprof_users, KPROF_USER, KPROF_TOP_USER_PC);
		kprof_put("SwitchKprof user %u/%u: %s", p + 1, nprocs, proc_line);
		kprof_flush();
	}
}
#else
static void
kprof_user_hot_spots(void)
{
	unsigned int procs[KPROF_TOP_USER], nprocs, top[KPROF_TOP_USER_PC], ntop;

	for (unsigned int i = 0; i < KPROF_TASKS; i++) {
		kprof_vals[i] = kprof_tasks[i].used ? kprof_tasks[i].user : 0;
	}
	nprocs = kprof_top(kprof_vals, KPROF_TASKS, procs, KPROF_TOP_USER);
	for (unsigned int p = 0; p < nprocs; p++) {
		const struct kprof_task *t = &kprof_tasks[procs[p]];
		uint32_t other = 0;

		for (unsigned int i = 0; i < KPROF_USER; i++) {
			bool mine = kprof_users[i].key != 0 && kprof_users[i].pid == t->pid;

			kprof_vals[i] = (mine && kprof_users[i].key != KPROF_U_OTHER) ? kprof_users[i].samples : 0;
			if (mine && kprof_users[i].key == KPROF_U_OTHER) {
				other = kprof_users[i].samples;
			}
		}
		ntop = kprof_top(kprof_vals, KPROF_USER, top, KPROF_TOP_USER_PC);
		if (p % 2 == 0) {
			kprof_put("SwitchKprof user %u/%u:", p / 2 + 1, (nprocs + 1) / 2);
		}
		kprof_put(" %s[%d] %u:", t->name, t->pid, t->user);
		for (unsigned int i = 0; i < ntop; i++) {
			const struct kprof_user *u = &kprof_users[top[i]];

			kprof_put(" %s %llx %u,", (u->key & ~KPROF_U_ADDR) == KPROF_U_DSC ? "dsc" : "main+",
			    u->key & KPROF_U_ADDR, u->samples);
		}
		kprof_put(" other %u;", other);
		if (p % 2 == 1 || p + 1 == nprocs) {
			kprof_flush();
		}
	}
}
#endif

/*
 * A process keeps the name it had at its first sample of the window, and a spawn's first sample is often still
 * xpcproxy (it execs the daemon in place): once per summary, every process still alive gets its current name.
 */
extern void proc_starttime_kdp(void *p, uint64_t *tv_sec, uint64_t *tv_usec, uint64_t *abstime);

static struct kprof_cpu_last *
kprof_cpu_last_slot(struct kprof_cpu_last *table, uint64_t uniqueid, task_t task, bool insert)
{
	uint32_t i = kprof_hash(uniqueid, 11);

	for (unsigned int probe = 0; probe < KPROF_PROBES; probe++, i = (i + 1) & (KPROF_CPU_LAST - 1)) {
		if (table[i].uniqueid == uniqueid && table[i].task == (uintptr_t)task) {
			return &table[i];
		}
		if (table[i].uniqueid == 0) {
			if (!insert) {
				return NULL;
			}
			table[i].uniqueid = uniqueid;
			table[i].task = (uintptr_t)task;
			return &table[i];
		}
	}
	return NULL;
}

/*
 * r14-launch: also each process's exact CPU in this window (its recount lifetime user+system against the last
 * summary's; a process started since then counts its whole life and is marked fresh, with its age).
 */
static void
kprof_refresh_names(void)
{
	struct kprof_cpu_last *cur = kprof_cpu_last[kprof_cpu_last_cur ^ 1];
	struct kprof_cpu_last *prev = kprof_cpu_last[kprof_cpu_last_cur];
	clock_sec_t            now_s;
	clock_usec_t           now_us;
	task_t                 task;

	bzero(cur, KPROF_CPU_LAST * sizeof(cur[0]));
	clock_get_calendar_microtime(&now_s, &now_us);
	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		const int32_t                    pid = task_pid(task);
		const uint64_t                   id = get_task_uniqueid(task);
		/* a task without a proc (UINT64_MAX) or kernel_task (0): keyed by pid instead, never 0 (the empty key) */
		const uint64_t                   uniqueid = (id == 0 || id == UINT64_MAX) ? (1ull << 63) | (uint32_t)pid : id;
		const struct recount_times_mach  times = recount_task_times(task);
		const uint64_t                   cpu = times.rtm_user + times.rtm_system;
		const struct kprof_cpu_last     *was = kprof_cpu_last_slot(prev, uniqueid, task, false);
		struct kprof_cpu_last           *is = kprof_cpu_last_slot(cur, uniqueid, task, true);
		struct kprof_task               *t = NULL;
		uint32_t                         i = kprof_hash((uint64_t)(uint32_t)pid + 1, 8);

		if (is != NULL) {
			is->cpu_abs = cpu;
		}
		for (unsigned int probe = 0; probe < KPROF_PROBES && kprof_tasks[i].used; probe++, i = (i + 1) & 255) {
			if (kprof_tasks[i].pid == pid) {
				t = &kprof_tasks[i];
				task_best_name(task, t->name, sizeof(t->name));
				break;
			}
		}
		if (t == NULL) {
			continue;
		}
		t->cpu_abs = was == NULL ? cpu : cpu - MIN(cpu, was->cpu_abs);
		if (was == NULL && kprof_cpu_last_primed && !(uniqueid >> 63)) {
			void    *p = get_bsdtask_info(task);
			uint64_t sec = 0, usec = 0;

			t->fresh = true;
			if (p != NULL) {
				proc_starttime_kdp(p, &sec, &usec, NULL);
			}
			if (sec != 0 && ((uint64_t)now_s > sec || ((uint64_t)now_s == sec && (uint64_t)now_us >= usec))) {
				t->age_ms = (uint32_t)MIN(((uint64_t)now_s - sec) * 1000 + (uint64_t)now_us / 1000 - usec / 1000,
				    UINT32_MAX);
			}
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);
	kprof_cpu_last_cur ^= 1;
	kprof_cpu_last_primed = true;
}

static uint64_t
kprof_abs_ms(uint64_t abs)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(abs, &ns);
	return ns / NSEC_PER_MSEC;
}

/* r14-launch: the processes that waited most (page-ins, cached-read misses) this window, with their exact CPU. */
static void
kprof_waits_line(void)
{
	unsigned int top[8], ntop, lines;

	for (unsigned int i = 0; i < KPROF_TASKS; i++) {
		kprof_vals[i] = kprof_tasks[i].used ? kprof_tasks[i].pagein_abs + kprof_tasks[i].read_abs : 0;
	}
	ntop = kprof_top(kprof_vals, KPROF_TASKS, top, 8);
	lines = (ntop + 3) / 4;   /* four processes per line: each line stays under KPROF_LINE */
	for (unsigned int line = 0; line < lines; line++) {
		kprof_put("SwitchKprof waits %u/%u (cpu ms; page-ins n/KiB/ms, cx: of them from a cryptex n/ms; read misses "
		    "n/ms; decmpfs page-ins):", line + 1, lines);
		for (unsigned int i = line * 4; i < MIN(ntop, (line + 1) * 4); i++) {
			const struct kprof_task *t = &kprof_tasks[top[i]];

			kprof_put(" %s[%d] cpu %llu, pin %u/%u/%llu cx %u/%llu, rd %u/%llu, dz %u", t->name, t->pid,
			    kprof_abs_ms(t->cpu_abs), t->pageins, t->pagein_kib, kprof_abs_ms(t->pagein_abs), t->image_pageins,
			    kprof_abs_ms(t->image_abs), t->reads, kprof_abs_ms(t->read_abs), t->decmpfs);
			if (t->fresh) {
				kprof_put(", new +%u ms", t->age_ms);
			}
			kprof_put(";");
		}
		kprof_flush();
	}
}

/* r14-launch, r17-launch: the cryptex images' reads this window: on the direct read map (SwitchCryptex.cpp), or
 * through the image files when DiskImages serves them (vfs_cluster.c switch_image_window). */
extern void switch_cluster_image_stats(uint64_t stats[9]);

static void
kprof_image_line(void)
{
	static uint64_t last[9];
	uint64_t        now[9], d[9];

	switch_cluster_image_stats(now);
	for (unsigned int i = 0; i < 9; i++) {
		d[i] = now[i] - last[i];
		last[i] = now[i];
	}
	if (d[6] != 0) {
		kprof_put("SwitchKprof image: %llu direct reads (%llu KiB) in %llu card reads", d[6], d[7] >> 10, d[8]);
		kprof_flush();
	}
	if (d[0] == 0 && d[4] == 0) {
		return;
	}
	kprof_put("SwitchKprof image: %llu cached reads, %llu missed, %llu windows read ahead (%llu KiB), "
	    "%llu card reads (%llu KiB)", d[0], d[1], d[2], d[3] >> 10, d[4], d[5] >> 10);
	kprof_flush();
}

/* The same processes' user samples by 1 MiB region, one line each: which images (SwiftUI, CoreText, libobjc...) a flat
 * profile is in, where the 64-byte hot spots above cover only its busiest tenth (r12-widgets). */
#if SWITCH_KPROF_ROLE_ON
static void
kprof_user_images(void)
{
	static char  img_line[KPROF_LINE];
	unsigned int procs[KPROF_TOP_USER], nprocs;

	for (unsigned int i = 0; i < KPROF_TASKS; i++) {
		kprof_vals[i] = kprof_tasks[i].used ? kprof_tasks[i].user : 0;
	}
	nprocs = kprof_top(kprof_vals, KPROF_TASKS, procs, KPROF_TOP_USER);
	for (unsigned int p = 0; p < nprocs; p++) {
		const struct kprof_task *t = &kprof_tasks[procs[p]];
		const uint32_t           main_user = MIN(t->user_main, t->user);
		const uint32_t           worker_user = t->user - main_user;

		if (main_user > 0) {
			switch_kprof_format_images_role(img_line, sizeof(img_line), t->name, t->pid, true, main_user, t->user,
			    kprof_images, KPROF_IMAGES, KPROF_TOP_IMAGES, kprof_images_lost);
			os_log(OS_LOG_DEFAULT, "%s", img_line);
		}
		if (worker_user > 0) {
			switch_kprof_format_images_role(img_line, sizeof(img_line), t->name, t->pid, false, worker_user,
			    t->user, kprof_images, KPROF_IMAGES, KPROF_TOP_IMAGES, kprof_images_lost);
			os_log(OS_LOG_DEFAULT, "%s", img_line);
		}
	}
}
#else
static void
kprof_user_images(void)
{
	unsigned int procs[KPROF_TOP_USER], nprocs, top[KPROF_TOP_IMAGES], ntop;

	for (unsigned int i = 0; i < KPROF_TASKS; i++) {
		kprof_vals[i] = kprof_tasks[i].used ? kprof_tasks[i].user : 0;
	}
	nprocs = kprof_top(kprof_vals, KPROF_TASKS, procs, KPROF_TOP_USER);
	for (unsigned int p = 0; p < nprocs; p++) {
		const struct kprof_task *t = &kprof_tasks[procs[p]];
		uint32_t                 logged = 0;

		for (unsigned int i = 0; i < KPROF_IMAGES; i++) {
			kprof_vals[i] = (kprof_images[i].key != 0 && kprof_images[i].pid == t->pid) ? kprof_images[i].samples : 0;
		}
		ntop = kprof_top(kprof_vals, KPROF_IMAGES, top, KPROF_TOP_IMAGES);
		kprof_put("SwitchKprof images: %s[%d] %u:", t->name, t->pid, t->user);
		for (unsigned int i = 0; i < ntop; i++) {
			const struct kprof_user *r = &kprof_images[top[i]];
			const uint64_t           kind = r->key & ~KPROF_U_ADDR;

			kprof_put(" %s %llx %u,", kind == KPROF_U_DSC ? "dsc" : kind == KPROF_U_MAIN ? "main+" : "other",
			    (r->key & KPROF_U_ADDR) >> 20, r->samples);
			logged += r->samples;
		}
		kprof_put(" rest %u; lost %u", t->user - MIN(t->user, logged), kprof_images_lost);
		kprof_flush();
	}
}
#endif

/* --- r17-faults: the faults lines ---------------------------------------------------------------------------------- */

static void
kprof_faults_merge(struct kprof_faults *dst, const struct kprof_faults *src)
{
	for (unsigned int k = 0; k < KPROF_F_CLASSES; k++) {
		dst->n[k] += src->n[k];
		dst->abs[k] += src->abs[k];
		if (src->max_abs[k] > dst->max_abs[k]) {
			dst->max_abs[k] = src->max_abs[k];
			dst->max_tid[k] = src->max_tid[k];
			dst->max_pid[k] = src->max_pid[k];
		}
	}
	dst->wired += src->wired;
	dst->kernel += src->kernel;
	dst->dsc += src->dsc;
	dst->copyio += src->copyio;
	dst->purgeable += src->purgeable;
	dst->fast_af += src->fast_af;
	dst->fast_mod += src->fast_mod;
}

static uint64_t
kprof_abs_us(uint64_t abs)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(abs, &ns);
	return ns / NSEC_PER_USEC;
}

static const char *
kprof_where(uint64_t key)
{
	switch (key & ~KPROF_U_ADDR) {
	case KPROF_U_DSC:
		return "dsc ";
	case KPROF_U_MAIN:
		return "main+";
	default:
		return "other ";
	}
}

/*
 * The window's faults: by class system-wide with XNU's vm_statistics64 deltas (vm_stat's counters: what the GPU
 * driver's mem line reports as "faults"), then the processes with the most, then where the top zero-fillers took them.
 */
static void
kprof_faults_lines(uint64_t window)
{
	static const char *const names[KPROF_F_CLASSES] = { "zf", "cow", "soft", "device", "file", "cmp", "failed" };
	static uint64_t          last[9];
	static bool              primed;
	const struct kprof_faults *f = &kprof_fault_sum;
	const uint64_t           now[9] = {
		counter_load(&vm_statistics_faults), counter_load(&vm_statistics_zero_fill_count),
		counter_load(&vm_statistics_cow_faults), counter_load(&vm_statistics_pageins),
		counter_load(&vm_statistics_reactivations), counter_load(&vm_statistics_decompressions),
		counter_load(&vm_statistics_compressions), counter_load(&vm_statistics_purges),
		counter_load(&vm_statistics_swapins),
	};
	uint64_t                 d[9], total = 0, zf_seen;
	unsigned int             top[KPROF_TOP_FAULTS], ntop, lines;

	for (unsigned int i = 0; i < 9; i++) {
		d[i] = now[i] - last[i];
		last[i] = now[i];
	}
	if (!primed) {
		primed = true;      /* the first window would count the whole boot so far */
		return;
	}
	for (unsigned int k = 0; k < KPROF_F_CLASSES; k++) {
		total += f->n[k];
	}
	if (window == 0 || (total == 0 && d[0] == 0 && f->fast_af + f->fast_mod == 0)) {
		return;
	}
	kprof_put("SwitchKprof faults: %llu/s (n, ms in vm_fault, longest us):", total * NSEC_PER_SEC / window);
	for (unsigned int k = 0; k < KPROF_F_CLASSES; k++) {
		kprof_put("%s %s %u %llu %llu", k ? "," : "", names[k], f->n[k], kprof_abs_ms(f->abs[k]),
		    kprof_abs_us(f->max_abs[k]));
	}
	zf_seen = f->n[KPROF_F_ZF];
	kprof_put("; wired %u, kernel map %u, shared region %u, copyio %u, purgeable zf %u; "
	    "fast %llu/s (af %u, mod %u); xnu: faults %llu (wire_fast %lld), zero_fill %llu (outside vm_fault %lld), cow %llu, "
	    "pageins %llu, reactivations %llu, decompressions %llu, compressions %llu, purges %llu, swapins %llu; free %u",
	    f->wired, f->kernel, f->dsc, f->copyio, f->purgeable,
	    (uint64_t)(f->fast_af + f->fast_mod) * NSEC_PER_SEC / window, f->fast_af, f->fast_mod,
	    d[0], (long long)d[0] - (long long)total, d[1], (long long)d[1] - (long long)zf_seen, d[2], d[3], d[4], d[5],
	    d[6], d[7], d[8], vm_page_free_count);
	kprof_flush();

	lines = 0;
	for (unsigned int k = 0; k < KPROF_F_CLASSES; k++) {
		char owner[24];

		if (kprof_abs_ms(f->max_abs[k]) < 10) {
			continue;
		}
		kprof_put("%s %s %llu ms %s[%d] thread 0x%llx", lines++ ? "," : "SwitchKprof longest faults (10 ms or more):",
		    names[k], kprof_abs_ms(f->max_abs[k]),
		    kprof_task_name((uint64_t)(uint32_t)f->max_pid[k], owner, sizeof(owner)), f->max_pid[k], f->max_tid[k]);
	}
	if (lines) {
		kprof_flush();
	}

	for (unsigned int i = 0; i < KPROF_TASKS; i++) {
		const struct kprof_task *t = &kprof_tasks[i];
		uint64_t                 n = 0;

		for (unsigned int k = 0; t->used && k < KPROF_F_CLASSES; k++) {
			n += t->faults[k];
		}
		kprof_vals[i] = n;
	}
	ntop = kprof_top(kprof_vals, KPROF_TASKS, top, KPROF_TOP_FAULTS);
	lines = (ntop + 3) / 4;
	for (unsigned int line = 0; line < lines; line++) {
		kprof_put("SwitchKprof faults by process %u/%u (n: zf cow soft device file cmp failed, ms; wired, shared "
		    "region, copyio; fast af/mod):", line + 1, lines);
		for (unsigned int i = line * 4; i < MIN(ntop, (line + 1) * 4); i++) {
			const struct kprof_task *t = &kprof_tasks[top[i]];

			kprof_put(" %s[%d] %llu:", t->name, t->pid, kprof_vals[top[i]]);
			for (unsigned int k = 0; k < KPROF_F_CLASSES; k++) {
				kprof_put(" %u", t->faults[k]);
			}
			kprof_put(", %llu ms; %u, %u, %u; %u/%u;", kprof_abs_ms(t->fault_abs), t->fault_wired, t->fault_dsc,
			    t->fault_copyio, t->fast_af, t->fast_mod);
		}
		kprof_flush();
	}

	/* the two processes with the most zero fills: their sites, one line each */
	for (unsigned int i = 0; i < KPROF_TASKS; i++) {
		kprof_vals[i] = kprof_tasks[i].used ? kprof_tasks[i].faults[KPROF_F_ZF] : 0;
	}
	ntop = kprof_top(kprof_vals, KPROF_TASKS, top, 2);
	for (unsigned int p = 0; p < ntop; p++) {
		const struct kprof_task *t = &kprof_tasks[top[p]];
		unsigned int             sites[KPROF_TOP_SITES], nsites;
		uint32_t                 logged = 0;

		for (unsigned int i = 0; i < KPROF_SITES; i++) {
			kprof_vals[KPROF_TASKS + i] = (kprof_sites[i].key != 0 && kprof_sites[i].pid == t->pid) ?
			    kprof_sites[i].n : 0;
		}
		nsites = kprof_top(kprof_vals + KPROF_TASKS, KPROF_SITES, sites, KPROF_TOP_SITES);
		kprof_put("SwitchKprof zf sites: %s[%d] %u (pc <majority lr n):", t->name, t->pid,
		    t->faults[KPROF_F_ZF]);
		for (unsigned int i = 0; i < nsites; i++) {
			const struct kprof_site *s = &kprof_sites[sites[i]];
			const uint64_t           key = s->key & ~KPROF_U_COPYIO;

			kprof_put(" %s%s%llx <%s%llx %u,", (s->key & KPROF_U_COPYIO) ? "k " : "", kprof_where(key),
			    key & KPROF_U_ADDR & ~KPROF_U_COPYIO, kprof_where(s->lr), s->lr & KPROF_U_ADDR, s->n);
			logged += s->n;
		}
		kprof_put(" rest %u", t->faults[KPROF_F_ZF] - MIN(t->faults[KPROF_F_ZF], logged));
		kprof_flush();
	}
}

extern uint64_t switch_ipis_received[][5];   /* cpu_common.c */
extern uint64_t switch_sched_ipis[7][2];      /* sched_prim.c, by sched_ipi_event_t */

/* The IPIs each CPU took by kind, and the scheduler's IPIs by why they were sent, per second over the window. */
static void
kprof_ipis(uint64_t window)
{
	static const char *const events[7] = { "?", "bound", "preempt", "smt", "spill", "rebalance", "rt" };
	static uint64_t last_received[KPROF_MAX_CPUS][5], last_sched[7][2];

	kprof_put("SwitchKprof ipis per s, taken (ast/xcall/timer/wake/other):");
	for (unsigned int c = 0; c < kprof_ncpus; c++) {
		kprof_put("%s", c ? "," : "");
		for (unsigned int k = 0; k < 5; k++) {
			const uint64_t n = os_atomic_load(&switch_ipis_received[c][k], relaxed);
			kprof_put("%s%llu", k ? "/" : " ", (n - last_received[c][k]) * NSEC_PER_SEC / window);
			last_received[c][k] = n;
		}
	}
	kprof_put("; scheduler sent to running/idle CPUs:");
	for (unsigned int e = 0; e < 7; e++) {
		const uint64_t running = os_atomic_load(&switch_sched_ipis[e][0], relaxed);
		const uint64_t idle = os_atomic_load(&switch_sched_ipis[e][1], relaxed);
		if (running != last_sched[e][0] || idle != last_sched[e][1]) {
			kprof_put(" %s %llu/%llu", events[e], (running - last_sched[e][0]) * NSEC_PER_SEC / window,
			    (idle - last_sched[e][1]) * NSEC_PER_SEC / window);
		}
		last_sched[e][0] = running;
		last_sched[e][1] = idle;
	}
	kprof_flush();
}

static void
kprof_summary(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t     now, window_abs, window, samples = 0, user = 0, kernel = 0, kernel_task_samples = 0, lost = 0;
	unsigned int top[KPROF_TOP_FNS], ntop;
	char         owner[24];

	cpu_broadcast_xcall_simple(TRUE, kprof_flip_cpu, NULL);
	now = mach_absolute_time();
	window_abs = now - kprof_last_summary;
	kprof_last_summary = now;
	absolutetime_to_nanoseconds(window_abs, &window);

	bzero(kprof_fns, KPROF_FNS * sizeof(kprof_fns[0]));
	bzero(kprof_tasks, KPROF_TASKS * sizeof(kprof_tasks[0]));
	bzero(kprof_users, KPROF_USER * sizeof(kprof_users[0]));
	bzero(kprof_images, KPROF_IMAGES * sizeof(kprof_images[0]));
	kprof_images_lost = 0;
	bzero(kprof_masks, KPROF_MASKED * sizeof(kprof_masks[0]));
	bzero(kprof_sites, KPROF_SITES * sizeof(kprof_sites[0]));
	bzero(&kprof_fault_sum, sizeof(kprof_fault_sum));
	for (unsigned int c = 0; c < kprof_ncpus; c++) {
		struct kprof_cpu *kc = &kprof_cpus[c];
		struct kprof_buf *b = kc->buf[kc->active ^ 1];

		kprof_faults_merge(&kprof_fault_sum, &b->faults);
		for (unsigned int i = 0; i < KPROF_CPU_SITES; i++) {
			const struct kprof_site *src = &b->site[i];
			struct kprof_site       *dst;

			if (src->key == 0) {
				continue;
			}
			dst = kprof_site_slot(kprof_sites, 10, src->pid, src->key);
			if (dst != NULL) {
				dst->n += src->n;
				kprof_vote_merge(&dst->lr, &dst->lr_votes, src->lr, src->lr_votes);
			}
		}
		samples += b->samples;
		user += b->user;
		kernel += b->kernel;
		kernel_task_samples += b->kernel_task;
		lost += b->lost;
		for (unsigned int i = 0; i < KPROF_CPU_FNS; i++) {
			const struct kprof_fn *src = &b->fn[i];
			struct kprof_fn       *dst;

			if (src->key == 0) {
				continue;
			}
			dst = kprof_fn_slot(kprof_fns, 12, src->key);
			if (dst == NULL) {
				lost += src->samples;
				continue;
			}
			dst->samples += src->samples;
			dst->kernel_task += src->kernel_task;
			kprof_vote_merge(&dst->caller, &dst->caller_votes, src->caller, src->caller_votes);
			kprof_vote_merge(&dst->owner, &dst->owner_votes, src->owner, src->owner_votes);
		}
		for (unsigned int i = 0; i < KPROF_CPU_MASKED; i++) {
			const struct kprof_masked *src = &b->masked[i];
			struct kprof_masked       *dst;

			if (src->chain[0] == 0) {
				continue;
			}
			dst = kprof_masked_slot(kprof_masks, 10, src->chain);
			if (dst != NULL) {
				dst->samples += src->samples;
				dst->kernel_task += src->kernel_task;
			}
		}
#if SWITCH_KPROF_ROLE_ON
		switch_kprof_user_merge(kprof_users, 13, b->user_pc, KPROF_CPU_USER, NULL);
		kprof_images_lost += b->image_lost;
		switch_kprof_user_merge(kprof_images, 10, b->image, KPROF_CPU_IMAGES, &kprof_images_lost);
#else
		for (unsigned int i = 0; i < KPROF_CPU_USER; i++) {
			const struct kprof_user *src = &b->user_pc[i];
			struct kprof_user       *dst;

			if (src->key == 0) {
				continue;
			}
			dst = kprof_user_slot(kprof_users, 13, src->pid, src->key);
			if (dst != NULL) {
				dst->samples += src->samples;
			}
		}
		kprof_images_lost += b->image_lost;
		for (unsigned int i = 0; i < KPROF_CPU_IMAGES; i++) {
			const struct kprof_user *src = &b->image[i];
			struct kprof_user       *dst;

			if (src->key == 0) {
				continue;
			}
			dst = kprof_user_slot(kprof_images, 10, src->pid, src->key);
			if (dst != NULL) {
				dst->samples += src->samples;
			} else {
				kprof_images_lost += src->samples;
			}
		}
#endif
		for (unsigned int i = 0; i < KPROF_CPU_TASKS; i++) {
			const struct kprof_task *src = &b->task[i];
			struct kprof_task       *dst;

			if (!src->used) {
				continue;
			}
			dst = kprof_task_slot(kprof_tasks, 8, src->pid);
			if (dst == NULL) {
				continue;
			}
			dst->user += src->user;
#if SWITCH_KPROF_ROLE_ON
			dst->user_main += src->user_main;
#endif
			dst->kernel += src->kernel;
			dst->pageins += src->pageins;
			dst->pagein_kib += src->pagein_kib;
			dst->image_pageins += src->image_pageins;
			dst->reads += src->reads;
			dst->read_kib += src->read_kib;
			dst->decmpfs += src->decmpfs;
			dst->decmpfs_kib += src->decmpfs_kib;
			dst->pagein_abs += src->pagein_abs;
			dst->image_abs += src->image_abs;
			dst->read_abs += src->read_abs;
			for (unsigned int k = 0; k < KPROF_F_CLASSES; k++) {
				dst->faults[k] += src->faults[k];
			}
			dst->fault_abs += src->fault_abs;
			dst->fault_wired += src->fault_wired;
			dst->fault_dsc += src->fault_dsc;
			dst->fault_copyio += src->fault_copyio;
			dst->fast_af += src->fast_af;
			dst->fast_mod += src->fast_mod;
			if (dst->name[0] == '\0') {
				strlcpy(dst->name, src->name, sizeof(dst->name));
			}
		}
		bzero(b, sizeof(*b));
	}

	kprof_refresh_names();
	kprof_put("SwitchKprof: %llu ms, %u cpus busy/irq %%:", window / NSEC_PER_MSEC, kprof_ncpus);
	for (unsigned int c = 0; c < kprof_ncpus; c++) {
		processor_t          processor = processor_array[c];
		struct recount_usage usage = {};
		uint64_t             idle, irq, idle_ns, irq_ns;

		if (processor == PROCESSOR_NULL) {
			continue;
		}
		recount_processor_usage(&processor->pr_recount, &usage, &idle);
		irq = processor->pr_recount.rpr_interrupt_duration_mach;
		absolutetime_to_nanoseconds(idle - MIN(idle, kprof_cpus[c].last_idle), &idle_ns);
		absolutetime_to_nanoseconds(irq - MIN(irq, kprof_cpus[c].last_irq), &irq_ns);
		kprof_cpus[c].last_idle = idle;
		kprof_cpus[c].last_irq = irq;
		if (!kprof_cpus[c].primed) {
			kprof_cpus[c].primed = true;
			kprof_put(" -");
			continue;
		}
		kprof_put(" %llu/%llu.%llu", (window - MIN(window, idle_ns)) * 100 / window, irq_ns * 100 / window,
		    irq_ns * 1000 / window % 10);
	}
	kprof_put("; irq/ipi/exc per s:");
	for (unsigned int c = 0; c < kprof_ncpus; c++) {
		cpu_data_t *cdp = (cpu_data_t *)CpuDataEntries[c].cpu_data_vaddr;
		uint64_t    irqs, ipis, exc;

		if (cdp == NULL) {
			continue;
		}
		irqs = cdp->cpu_stat.irq_ex_cnt;
		ipis = cdp->cpu_stat.ipi_cnt;
		exc = cdp->cpu_stat.data_ex_cnt + cdp->cpu_stat.instr_ex_cnt;
		kprof_put(" %llu/%llu/%llu", (irqs - kprof_cpus[c].last_irqs) * NSEC_PER_SEC / window,
		    (ipis - kprof_cpus[c].last_ipis) * NSEC_PER_SEC / window, (exc - kprof_cpus[c].last_exc) * NSEC_PER_SEC / window);
		kprof_cpus[c].last_irqs = irqs;
		kprof_cpus[c].last_ipis = ipis;
		kprof_cpus[c].last_exc = exc;
	}
	kprof_put("; samples %llu (%llu ms each): user %llu, kernel %llu (kernel_task %llu), lost %llu", samples,
	    KPROF_PERIOD_NS / NSEC_PER_MSEC, user, kernel, kernel_task_samples, lost);
	const uint32_t gov_khz = switch_perf_cpu_khz();
	if (gov_khz) {
		kprof_put("; governor MHz: %u", gov_khz / 1000);
	}
	kprof_flush();
	kprof_ipis(window);

	for (unsigned int i = 0; i < KPROF_TASKS; i++) {
		kprof_vals[i] = kprof_tasks[i].used ? (uint64_t)kprof_tasks[i].user + kprof_tasks[i].kernel : 0;
	}
	ntop = kprof_top(kprof_vals, KPROF_TASKS, top, KPROF_TOP_TASKS);
	kprof_put("SwitchKprof tasks (user+kernel samples=exact CPU):");
	for (unsigned int i = 0; i < ntop; i++) {
		const struct kprof_task *t = &kprof_tasks[top[i]];
		kprof_put("%s %s[%d] %u+%u=%llums", i ? "," : "", t->name, t->pid, t->user, t->kernel, kprof_abs_ms(t->cpu_abs));
		if (t->fresh) {
			kprof_put(" new +%ums", t->age_ms);
		}
	}
	kprof_flush();

	for (unsigned int i = 0; i < KPROF_FNS; i++) {
		kprof_vals[i] = kprof_fns[i].samples;
	}
	ntop = kprof_top(kprof_vals, KPROF_FNS, top, KPROF_TOP_FNS);
	for (unsigned int half = 0; half < 2; half++) {
		kprof_put("SwitchKprof kernel %u/2 (fn samples kernel_task <caller @owner):", half + 1);
		for (unsigned int i = half * KPROF_TOP_FNS / 2; i < MIN(ntop, (half + 1) * KPROF_TOP_FNS / 2); i++) {
			const struct kprof_fn *f = &kprof_fns[top[i]];

			kprof_put_addr(" ", f->key);
			kprof_put(" %u %u", f->samples, f->kernel_task);
			kprof_put_addr(" <", f->caller);
			kprof_put(" @%s;", kprof_task_name(f->owner, owner, sizeof(owner)));
		}
		kprof_flush();
	}

	for (unsigned int i = 0; i < KPROF_MASKED; i++) {
		kprof_vals[i] = kprof_masks[i].samples;
	}
	ntop = kprof_top(kprof_vals, KPROF_MASKED, top, MIN(KPROF_TOP_MASKED, KPROF_TOP_FNS));
	if (ntop != 0) {
		kprof_put("SwitchKprof masked (samples kernel_task: masking code < its callers):");
		for (unsigned int i = 0; i < ntop; i++) {
			const struct kprof_masked *m = &kprof_masks[top[i]];

			kprof_put(" %u %u", m->samples, m->kernel_task);
			for (unsigned int j = 0; j < 3 && m->chain[j] != 0; j++) {
				kprof_put_addr(j ? " <" : " ", m->chain[j]);
			}
			kprof_put(";");
		}
		kprof_flush();
	}

	kprof_decmpfs_line();
	kprof_waits_line();
	kprof_image_line();
	kprof_faults_lines(window);
	kprof_kernel_task_threads(window);
	kprof_user_hot_spots();
	kprof_user_images();
	thread_call_enter_delayed_with_leeway(kprof_call, NULL, now + kprof_window, kprof_window / 10,
	    THREAD_CALL_DELAY_LEEWAY);
}

/* --- boot ---------------------------------------------------------------------------------------------------------- */

/* The kernel's LC_FUNCTION_STARTS, decoded while __LINKEDIT is still mapped (it is jettisoned at lockdown). */
static void
kprof_function_starts(void)
{
	kernel_mach_header_t           *mh = &_mh_execute_header;
	kernel_segment_command_t       *text = getsegbynamefromheader(mh, "__TEXT");
	kernel_segment_command_t       *exec = getsegbynamefromheader(mh, "__TEXT_EXEC");
	kernel_segment_command_t       *linkedit = getsegbynamefromheader(mh, "__LINKEDIT");
	const struct load_command      *lc = (const struct load_command *)(mh + 1);
	const struct linkedit_data_command *fs = NULL;
	const uint8_t                  *p, *end;
	uintptr_t                       slide;
	uint64_t                        off = 0;
	uint32_t                        n = 0;

	if (text == NULL || exec == NULL || linkedit == NULL) {
		return;
	}
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		if (lc->cmd == LC_FUNCTION_STARTS) {
			fs = (const struct linkedit_data_command *)lc;
		}
		lc = (const struct load_command *)((uintptr_t)lc + lc->cmdsize);
	}
	if (fs == NULL || fs->datasize == 0 || fs->dataoff < linkedit->fileoff ||
	    fs->dataoff + fs->datasize > linkedit->fileoff + linkedit->filesize) {
		return;
	}
	slide = (uintptr_t)mh - (uintptr_t)text->vmaddr;   /* 0 if the header is already slid */
	kprof_image = (uintptr_t)mh;
	kprof_text_start = (uintptr_t)exec->vmaddr + slide;
	kprof_text_end = kprof_text_start + (uintptr_t)exec->vmsize;
	p = (const uint8_t *)((uintptr_t)linkedit->vmaddr + slide + (fs->dataoff - linkedit->fileoff));
	end = p + fs->datasize;

	/* ULEB128 deltas from the image start; two passes: count, then fill */
	for (unsigned int pass = 0; pass < 2; pass++) {
		const uint8_t *q = p;

		off = 0;
		n = 0;
		while (q < end && *q != 0) {
			uint64_t delta = 0;
			unsigned int shift = 0;

			do {
				delta |= (uint64_t)(*q & 0x7f) << shift;
				shift += 7;
			} while ((*q++ & 0x80) && q < end && shift < 64);
			off += delta;
			if (off > UINT32_MAX) {
				break;
			}
			if (pass == 1) {
				kprof_starts[n] = (uint32_t)off;
			}
			n++;
		}
		if (pass == 0) {
			/* Large: kalloc_large refuses Z_NOFAIL (panics). Without the table, 128-byte buckets. */
			kprof_starts = kalloc_data(n * sizeof(uint32_t), Z_WAITOK);
			if (kprof_starts == NULL) {
				return;
			}
		}
	}
	kprof_nstarts = n;
}

static void
kprof_init(void)
{
	kprof_ncpus = MIN((unsigned int)ml_get_max_cpu_number() + 1, KPROF_MAX_CPUS);
	nanoseconds_to_absolutetime(KPROF_PERIOD_NS, &kprof_period);
	nanoseconds_to_absolutetime(KPROF_WINDOW_NS, &kprof_window);
	kprof_function_starts();
	kprof_unmask[0] = kprof_fn_key((uintptr_t)&ml_set_interrupts_enabled_with_debug);
	kprof_unmask[1] = kprof_fn_key((uintptr_t)&ml_set_interrupts_enabled);
	/*
	 * These tables are past kalloc's zone sizes, and kalloc_large panics on Z_NOFAIL ("trying to kalloc(Z_NOFAIL)
	 * with a large size"): a diagnostic must not take the boot down, so a refused table leaves kprof off.
	 */
	bool tables = true;
	for (unsigned int c = 0; c < kprof_ncpus; c++) {
		kprof_cpus[c].buf[0] = kalloc_data(sizeof(struct kprof_buf), Z_WAITOK | Z_ZERO);
		kprof_cpus[c].buf[1] = kalloc_data(sizeof(struct kprof_buf), Z_WAITOK | Z_ZERO);
		kprof_cpus[c].rng = 0x9e3779b9u * (c + 1);
		tables = tables && kprof_cpus[c].buf[0] != NULL && kprof_cpus[c].buf[1] != NULL;
	}
	kprof_fns = kalloc_data(KPROF_FNS * sizeof(kprof_fns[0]), Z_WAITOK | Z_ZERO);
	kprof_tasks = kalloc_data(KPROF_TASKS * sizeof(kprof_tasks[0]), Z_WAITOK | Z_ZERO);
	kprof_users = kalloc_data(KPROF_USER * sizeof(kprof_users[0]), Z_WAITOK | Z_ZERO);
	kprof_images = kalloc_data(KPROF_IMAGES * sizeof(kprof_images[0]), Z_WAITOK | Z_ZERO);
	kprof_threads[0] = kalloc_data(KPROF_THREADS * sizeof(struct kprof_thread), Z_WAITOK | Z_ZERO);
	kprof_threads[1] = kalloc_data(KPROF_THREADS * sizeof(struct kprof_thread), Z_WAITOK | Z_ZERO);
	kprof_vals = kalloc_data(KPROF_USER * sizeof(uint64_t), Z_WAITOK | Z_ZERO);
	kprof_masks = kalloc_data(KPROF_MASKED * sizeof(kprof_masks[0]), Z_WAITOK | Z_ZERO);
	kprof_sites = kalloc_data(KPROF_SITES * sizeof(kprof_sites[0]), Z_WAITOK | Z_ZERO);
	kprof_cpu_last[0] = kalloc_data(KPROF_CPU_LAST * sizeof(struct kprof_cpu_last), Z_WAITOK | Z_ZERO);
	kprof_cpu_last[1] = kalloc_data(KPROF_CPU_LAST * sizeof(struct kprof_cpu_last), Z_WAITOK | Z_ZERO);
	if (!tables || kprof_cpu_last[0] == NULL || kprof_cpu_last[1] == NULL || kprof_fns == NULL || kprof_tasks == NULL ||
	    kprof_users == NULL || kprof_images == NULL || kprof_threads[0] == NULL ||
	    kprof_threads[1] == NULL || kprof_vals == NULL || kprof_masks == NULL || kprof_sites == NULL) {
		os_log(OS_LOG_DEFAULT, "SwitchKprof: off (a sample table could not be allocated)");
		return;
	}
	kprof_call = thread_call_allocate_with_options(kprof_summary, NULL, THREAD_CALL_PRIORITY_USER,
	    THREAD_CALL_OPTIONS_ONCE);
	kprof_last_summary = mach_absolute_time();
	os_atomic_store(&kprof_ready, true, release);
	thread_call_enter_delayed_with_leeway(kprof_call, NULL, kprof_last_summary + kprof_window, kprof_window / 10,
	    THREAD_CALL_DELAY_LEEWAY);
	os_log(OS_LOG_DEFAULT, "SwitchKprof: sampling every %llu ms of CPU work, %u kernel functions%s, summary every %llu s",
	    KPROF_PERIOD_NS / NSEC_PER_MSEC, kprof_nstarts, kprof_nstarts ? "" : " (no LC_FUNCTION_STARTS: 128-byte buckets)",
	    KPROF_WINDOW_NS / NSEC_PER_SEC);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, kprof_init);

#else /* SWITCH_KPROF */

void
switch_kprof_expire(__unused void *param0, __unused void *param1)
{
}

static void
kprof_init(void)
{
	os_log(OS_LOG_DEFAULT, "SwitchKprof: off");
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, kprof_init);

#endif /* SWITCH_KPROF */

#endif /* DEVELOPMENT || DEBUG */
