"""Our code in XNU: which kernel/switch sources each XNU component compiles, and where each one links.

Both kernels compile every entry, with the recipe of the component it is listed under (that component's XNU flags,
includes and, in release, LTO) plus its own copts, and link it right after the Apple object it names. Every XNU object
also gets -I kernel/switch/include (the hook headers) and the selected XNU's version macro (XNU_10063_141_1,
XNU_12377_121_6), so code that differs between versions is `#if XNU_<version>` here, never a second copy.
"""

SWITCH_ROOT = "kernel/switch"

# The IPSW kind's kernel window base (<switch/boot_layout.h>), for the objects that include that header only: the
# xnu_objects rules expand {kernel_virt_base} from ipsw/pins.bzl's derived kernel_virt_base.
_KERNEL_VIRT_BASE = "-DSWITCH_KERNEL_VIRT_BASE={kernel_virt_base}"

# component: {Apple object: our copts}, after the object's own.
SWITCH_APPLE_COPTS = {
    "osfmk": {
        "osfmk/arm64/start.s": [_KERNEL_VIRT_BASE],
    },
}

# The arm64e translator runs inside exec, mmap and page faults, deep in a 16 KB kernel stack: its scratch is allocated
# (struct a64x_walk, struct a64x_layout), and a frame over 1 KB in it fails the build.
_A64X_COPTS = ["-Wframe-larger-than=1024", "-Werror=frame-larger-than"]

# component: [(the Apple object it links after, the source below kernel/switch (or //<path> for one elsewhere in the
# repository), its own copts[, the XNU versions that build it, when not all])]
SWITCH_OBJECTS = {
    "bsd": [
        ("bsd/kern/bsd_init.c", "boot/bsd_init.c", []),
        ("bsd/kern/kern_exec.c", "boot/prefs.c", []),
        ("bsd/kern/kern_exec.c", "boot/migration_home.c", []),
        ("bsd/kern/kern_exec.c", "security/app_code.c", []),
        ("bsd/kern/kern_exec.c", "security/bootstrap.c", ["-include", "{owner_header}"]),
        ("bsd/kern/kern_exec.c", "security/bootstrap_apps.c", ["-include", "{owner_header}"]),
        ("bsd/kern/kern_exec.c", "security/bootstrap_persona.c", []),
        ("bsd/kern/kern_exec.c", "security/bootstrap_dyld.c", ["-include", "{owner_header}"]),
        ("bsd/kern/ubc_subr.c", "security/bootstrap_signature.c", []),
        ("bsd/kern/kern_exec.c", "security/entitlements_factory.c", []),
        ("bsd/kern/kern_exec.c", "boot/icon_cache.c", []),
        ("bsd/kern/kern_exec.c", "boot/icon_cache_sweep.c", [], ["12377.121.6"]),
        ("bsd/kern/kern_exit.c", "diag/exit.c", []),
        ("bsd/kern/kdebug.c", "recorder/recorder.c", []),
        ("bsd/kern/kern_event.c", "diag/workloop.c", []),
        ("bsd/net/necp.c", "diag/necp.c", [], ["12377.121.6"]),
        ("bsd/net/necp.c", "diag/necp_table.c", [], ["12377.121.6"]),
        ("bsd/vm/vm_unix.c", "diag/cryptex_map.c", [], ["12377.121.6"]),
        ("bsd/kern/lockdown_mode.c", "sysctl/lockdown.c", [], ["12377.121.6"]),
        ("bsd/kern/kern_shutdown.c", "shutdown/shutdown.c", []),
        ("bsd/vfs/vfs_lookup.c", "vfs/pretranslated.c", []),
        ("bsd/vfs/vfs_cluster.c", "vm/prefetch.c", ["-include", "{owner_header}"]),
        ("bsd/vfs/vfs_cluster.c", "vm/prefetch_save.c", []),
        ("bsd/vfs/kpi_vfs.c", "vfs/auth_fs.c", []),
        ("bsd/vfs/vfs_syscalls.c", "vfs/identity.c", []),
        ("bsd/vfs/vfs_syscalls.c", "vfs/fsctl_diag.c", []),
        ("bsd/vfs/vfs_attrlist.c", "vfs/protection.c", [], ["12377.121.6"]),
        # The translator's core is //translate/arm64e's (its host tools and tests build the same files).
        ("bsd/kern/mach_loader.c", "translate/a64x_xnu.c", _A64X_COPTS, ["12377.121.6"]),
        ("bsd/kern/mach_loader.c", "//translate/arm64e/a64x/a64x_classify.c", _A64X_COPTS, ["12377.121.6"]),
        ("bsd/kern/mach_loader.c", "//translate/arm64e/a64x/a64x_rewrite.c", _A64X_COPTS, ["12377.121.6"]),
        ("bsd/kern/mach_loader.c", "//translate/arm64e/a64x/a64x_image.c", _A64X_COPTS, ["12377.121.6"]),
        ("bsd/kern/kern_codesigning.c", "prelink/shims.c", [], ["12377.121.6"]),
    ],
    "iokit": [
        ("iokit/Kernel/IOBufferMemoryDescriptor.cpp", "iokit/fresh_memory.cpp", []),
        ("iokit/Kernel/IOMemoryDescriptor.cpp", "iokit/memory_attachment.cpp", []),
        ("iokit/Kernel/IOMemoryDescriptor.cpp", "iokit/park_mapping.cpp", []),
        ("iokit/Kernel/IOUserClient.cpp", "iokit/iosurface.cpp", []),
        ("iokit/Kernel/IOUserClient.cpp", "security/app_capabilities.cpp", []),
    ],
    "osfmk": [
        ("osfmk/vm/vm_map.c", "security/dyld_policy.c", [], ["12377.121.6"]),
        ("osfmk/vm/vm_map.c", "vm/park_vm.c", []),
        ("osfmk/arm/model_dep.c", "diag/threads.c", []),
        ("osfmk/ipc/ipc_kmsg.c", "diag/frames.c", []),
        ("osfmk/arm64/arm_vm_init.c", "ram/ram.c", [_KERNEL_VIRT_BASE]),
        ("osfmk/arm64/cpu.c", "idle/idle.c", []),
        ("osfmk/kern/timer_call.c", "sched/stats.c", []),
        ("osfmk/kern/timer_call.c", "sched/render.c", []),
        ("osfmk/kern/timer_call.c", "sched/cpu.c", []),
        ("osfmk/kern/timer_call.c", "sched/watch.c", []),
        ("osfmk/kern/timer_call.c", "sched/watch_write.c", []),
        ("osfmk/kern/timer_call.c", "sched/commit.c", []),
        ("osfmk/kern/sched_prim.c", "diag/stall.c", []),
        ("osfmk/kern/sched_prim.c", "diag/stall_snapshot.c", []),
        ("osfmk/kern/sched_prim.c", "diag/stall_chain.c", []),
        ("osfmk/kern/sched_prim.c", "diag/stall_frame.c", []),
        ("osfmk/kern/sched_prim.c", "diag/heartbeat.c", []),
        ("osfmk/kern/sched_prim.c", "sched/busy.c", []),
        ("osfmk/kern/sched_prim.c", "sched/user_turnstile.c", []),
        ("osfmk/kern/sched_prim.c", "diag/unnest.c", []),
        ("osfmk/vm/bsd_vm.c", "diag/pagein.c", []),
        ("osfmk/vm/vm_pageout.c", "vm/prefetch_vm.c", []),
        ("osfmk/kern/sched_prim.c", "recorder/watch.c", []),
        ("osfmk/kperf/kperf.c", "recorder/sample.c", []),
        ("osfmk/kern/exception.c", "diag/crash.c", []),
        ("osfmk/kern/host.c", "vm/stats.c", [], ["12377.121.6"]),
    ],
}

def _entries(component):
    """The component's entries for the XNU of the package calling (kernel/xnu-<version>/<component>)."""
    xnu = native.package_name().split("/")[1][len("xnu-"):]
    out = []
    for entry in SWITCH_OBJECTS.get(component, []):
        if len(entry) == 3 or xnu in entry[3]:
            out.append(entry[:3])
    return out

def switch_objects(component, objects):
    """A component's objects (its table, in link order) with its kernel/switch sources as "switch:" entries.

    Args:
        component: osfmk, bsd, ...
        objects: the component's objects, in link order.

    Returns:
        The objects, each kernel/switch source right after the Apple object its entry names.
    """
    out = list(objects)
    for after, src, _ in _entries(component):
        if after not in out:
            fail("kernel/switch/objects.bzl: {} links after {}, which {} does not build".format(src, after, component))
        at = out.index(after) + 1
        for _ in range(len(out)):
            if at < len(out) and out[at].startswith("switch:"):
                at += 1
        out.insert(at, "switch:" + src)
    return out

def switch_copts(component, copts):
    """A component's per-object copts with its kernel/switch sources' own and SWITCH_APPLE_COPTS."""
    out = dict(copts)
    for _, src, flags in _entries(component):
        out["switch:" + src] = flags
    for obj, flags in SWITCH_APPLE_COPTS.get(component, {}).items():
        out[obj] = out.get(obj, []) + flags
    return out

# The objects whose hooks <switch/recorder.h> expands under --//kernel:recorder, per component.
_RECORDER_OBJECTS = {
    "bsd": [
        "bsd/kern/kdebug.c",
        "bsd/kern/kdebug_common.c",
        "bsd/kern/kern_shutdown.c",
        "switch:recorder/recorder.c",
    ],
    "osfmk": [
        "osfmk/kperf/kperf.c",
        "osfmk/kperf/kptimer.c",
        "switch:diag/stall.c",
        "switch:recorder/sample.c",
        "switch:recorder/watch.c",
        "switch:sched/stats.c",
    ],
}

def with_recorder(component, copts):
    """A component's `copts` with SWITCH_RECORDER on the objects that expand the recorder's hooks (--//kernel:recorder).

    Args:
        component: osfmk, bsd, ...
        copts: the component's per-object copts.

    Returns:
        The copts with the define on those objects.
    """
    return copts | {obj: copts.get(obj, []) + ["-DSWITCH_RECORDER=1"] for obj in _RECORDER_OBJECTS.get(component, [])}

def with_kprof(component, copts):
    """A component's `copts` with SWITCH_KPROF for switch_kprof.c (--//kernel:kprof), the one object that reads it.

    Args:
        component: osfmk, bsd, ...
        copts: the component's per-object copts.

    Returns:
        The copts, osfmk's with the define on that object.
    """
    if component != "osfmk":
        return copts
    kprof = "osfmk/kern/switch_kprof.c"
    return copts | {kprof: copts.get(kprof, []) + ["-DSWITCH_KPROF=1"]}
