/*
 * SwitchBootBand: the boot's background daemons yield the CPU to SpringBoard and backboardd until the home screen is up.
 *
 * From launchd to "Dismissing boot logo" all four cores are 86-100% busy (r7p5/r7p6 kprof), and daemons that nothing on
 * the home screen's path waits for (health, home, reminders, Siri knowledge, Spotlight, assets, analytics, widget
 * extensions, ...) took about a quarter of that CPU. The clutch scheduler gives their default and utility buckets a share
 * of every core even while SpringBoard's threads are runnable. While the band is open, those processes are spawned with
 * the CPU half of DARWIN_BG (switch_task_set_lowpri_cpu, task_policy.c): their threads run in the background bucket,
 * which still gets its share of an otherwise busy core; their disk, network and timers are untouched; an importance boost
 * (a message from a boosted client, e.g. SpringBoard's XPC) lifts it; and a turnstile push from a waiter still raises the
 * thread that owns what the waiter needs.
 *
 * The band closes for good at the first of: backboardd setting "System Boot Complete" on IOPMrootDomain (SpringBoard
 * finished startup, ~3 s after the boot logo is dismissed; IOPMrootDomain kPowerEventSystemBootCompleted) or 120 s of
 * uptime, a bound System Boot Complete comes well inside (81 s on 25 Nov; at 60 s the bound closed the band while
 * SpringBoard still had 18 s to go to its logo). Every process it held is put back then. One log line says when and
 * how many. (The boot's full CPU and GPU clocks are not tied to the band: they end at System Boot Complete itself,
 * IOPMrootDomain's switch_perf_hint.)
 */

#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/processor.h>
#include <kern/task.h>
#include <kern/thread_call.h>
#include <os/log.h>
#include <string.h>

extern void switch_task_set_lowpri_cpu(task_t task, bool on);
int switch_boot_band_classify(const char *path);
void switch_boot_band_spawned(task_t task, int band_class);
void switch_boot_band_lift(const char *why);

#define BAND_MAX        128
#define BAND_NONE       0       /* image_params.ip_switch_band (zeroed with the exec's data) */
#define BAND_DAEMON     1
#define BAND_WIDGET     2
#define BAND_UPTIME_NS  120000000000ull

/* Executables (basenames) that nothing on SpringBoard's or backboardd's startup path waits for. */
static const char *const band_names[] = {
	"mobileassetd", "biomed", "healthd", "homed", "remindd", "routined", "cloudd", "siriknowledged", "siriinferenced",
	"siriactionsd", "suggestd", "searchd", "spotlightknowledged", "triald", "analyticsd", "PerfPowerServices",
	"axassetsd", "knowledgeconstructiond", "mlhostd", "tipsd", "languageassetd", "managedappdistributiond",
	"remotemanagementd", "itunescloudd", "duetexpertd", "linkd", "AppPredictionIntentsHelperService", "ReportCrash",
	"OTAUpdate", "SafariBookmarksSyncAgent", "webbookmarksd", "com.apple.MapKit.SnapshotService",
};

static LCK_GRP_DECLARE(band_lck_grp, "switch_boot_band");
static LCK_MTX_DECLARE(band_lock, &band_lck_grp);
static bool          band_closed;
static uint64_t      band_ids[BAND_MAX];
static unsigned int  band_count, band_refused, band_seen, band_widgets;
static thread_call_t band_call;

/* Widget extensions (WidgetKit .appex): rendered by chronod for the home screen after it is up. */
static bool
band_widget_extension(const char *path, const char *name)
{
	return strnstr(path, ".appex/", strlen(path)) != NULL &&
	       (strnstr(name, "Widget", strlen(name)) != NULL || strcmp(name, "NewsToday2") == 0 ||
	       strcmp(name, "NewsTag") == 0);
}

/*
 * exec_activate_image (kern_exec.c), on success, while the executable's path still exists: which band, if any, the image
 * belongs to. posix_spawn() and execve() apply it after the spawn attributes' policy (switch_boot_band_spawned), by
 * which time the path has been freed with the exec's strings area.
 */
int
switch_boot_band_classify(const char *path)
{
	const char *name;

	if (path == NULL || os_atomic_load(&band_closed, relaxed)) {
		return BAND_NONE;
	}
	name = strrchr(path, '/');
	name = name ? name + 1 : path;
	if (band_widget_extension(path, name)) {
		return BAND_WIDGET;
	}
	for (unsigned int i = 0; i < sizeof(band_names) / sizeof(band_names[0]); i++) {
		if (strcmp(name, band_names[i]) == 0) {
			return BAND_DAEMON;
		}
	}
	return BAND_NONE;
}

static void
band_timeout(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	switch_boot_band_lift("120 s of uptime");
}

/* posix_spawn and execve (kern_exec.c), after the spawn attributes' policy: a member spawned while the band is open
 * joins it. */
void
switch_boot_band_spawned(task_t task, int band_class)
{
	uint64_t now_ns;

	if (os_atomic_load(&band_closed, relaxed)) {
		return;
	}
	os_atomic_inc(&band_seen, relaxed);
	if (band_class == BAND_NONE) {
		return;
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
	lck_mtx_lock(&band_lock);
	if (band_closed) {
		lck_mtx_unlock(&band_lock);
		return;
	}
	if (now_ns >= BAND_UPTIME_NS) {
		lck_mtx_unlock(&band_lock);
		switch_boot_band_lift("120 s of uptime");
		return;
	}
	if (band_call == NULL) {
		uint64_t deadline;

		band_call = thread_call_allocate_with_options(band_timeout, NULL, THREAD_CALL_PRIORITY_KERNEL,
		    THREAD_CALL_OPTIONS_ONCE);
		nanoseconds_to_absolutetime(BAND_UPTIME_NS, &deadline);
		if (band_call != NULL) {
			thread_call_enter_delayed(band_call, deadline);
		}
	}
	if (band_count == BAND_MAX || band_call == NULL) {
		band_refused++;
		lck_mtx_unlock(&band_lock);
		return;
	}
	band_ids[band_count++] = get_task_uniqueid(task);
	if (band_class == BAND_WIDGET) {
		band_widgets++;
	}
	switch_task_set_lowpri_cpu(task, true);
	lck_mtx_unlock(&band_lock);
}

/* IOPMrootDomain's System Boot Complete, or the 120 s bound: every task still in the band gets its CPU back, once. */
void
switch_boot_band_lift(const char *why)
{
	task_t       held[BAND_MAX];
	unsigned int count = 0, members;
	uint64_t     now_ns;
	task_t       task;

	lck_mtx_lock(&band_lock);
	if (band_closed) {
		lck_mtx_unlock(&band_lock);
		return;
	}
	os_atomic_store(&band_closed, true, relaxed);
	members = band_count;
	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		uint64_t id = get_task_uniqueid(task);

		for (unsigned int i = 0; i < band_count; i++) {
			if (band_ids[i] == id) {
				task_reference(task);
				held[count++] = task;
				break;
			}
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);
	lck_mtx_unlock(&band_lock);

	for (unsigned int i = 0; i < count; i++) {
		switch_task_set_lowpri_cpu(held[i], false);
		task_deallocate(held[i]);
	}
	if (band_call != NULL) {
		thread_call_cancel(band_call);
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
	os_log(OS_LOG_DEFAULT, "SwitchBootBand: closed at %llu.%llu s of uptime (%s): %u processes held at background "
	    "CPU priority (%u widget extensions), %u still running put back, %u not held (table full); %u execs seen",
	    now_ns / NSEC_PER_SEC, now_ns % NSEC_PER_SEC / 100000000, why, members, band_widgets, count, band_refused,
	    os_atomic_load(&band_seen, relaxed));
}
