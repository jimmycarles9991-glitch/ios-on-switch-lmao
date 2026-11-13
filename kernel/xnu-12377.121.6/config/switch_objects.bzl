"""Switch's feature switches per XNU object: the -D/-U/-include each one gets before its conf/Makefile additions
(the 10063 tree's per-object options, same files and values)."""

SWITCH_COPTS = {
    "bsd/kern/bsd_init.c": [
        "-D",
        "XNU_SHENG_PMGR=1",
        "-D",
        "XNU_VERIFIED_ROOT_PROBE=1",
        "-D",
        "XNU_OWNER_ROOT_AUDIT=1",
        "-D",
        "XNU_OWNER_ROOT_TRUST=1",
        "-D",
        "XNU_SWITCH_VERIFIED_ROOT=1",
        "-D",
        "XNU_ADRENO_FIRMWARE_PROBE=0",
        "-D",
        "XNU_ADRENO_PM4_PROBE=0",
        "-D",
        "XNU_SMMU4K_PROBE=0",
        "-D",
        "XNU_CONSTANT_UPLOADS_PROBE=0",
        "-D",
        "XNU_NATIVE_LEGACY_DISPLAY=1",
        "-D",
        "XNU_NATIVE_OFFLINE_GPU=0",
    ],
    "bsd/kern/kern_exec.c": [
        "-D",
        "XNU_NATIVE_AIR_INIT=0",
        "-D",
        "XNU_OWNER_CACHE_MOUNT=1",
        "-D",
        "XNU_OWNER_CACHE_STARTUP=1",
        "-D",
        "XNU_OWNER_BOOT_MANIFEST=1",
        "-D",
        "XNU_OWNER_DATA_MOUNT=1",
        "-D",
        "XNU_NATIVE_RAM_BLOCK=1",
        "-D",
        "XNU_OWNER_CRYPTEX_FILES=0",
        "-D",
        "XNU_OWNER_UNION_SYSTEMOS=1",
        "-include",
        "{owner_header}",
    ],
    "bsd/kern/ubc_subr.c": [
        "-D",
        "XNU_NATIVE_CS_HASH_CACHE=1",
    ],
    "bsd/vm/vm_unix.c": [
        "-D",
        "XNU_OWNER_CACHE_CARRIER=0",
        "-D",
        "XNU_OWNER_CACHE_CARRIER_WRONG_UUID=0",
        "-include",
        "{owner_header}",
    ],
    "iokit/Kernel/IOUserClient.cpp": [
        "-D",
        "XNU_NATIVE_MOBILE_DISPLAY=0",
        "-D",
        "XNU_NATIVE_LEGACY_DISPLAY=1",
    ],
    "iokit/KernelConfigTables.cpp": [
        "-D",
        "XNU_NATIVE_AES=1",
        "-D",
        "XNU_NATIVE_RAM_BLOCK=1",
    ],
    "iokit/bsddev/IOKitBSDInit.cpp": [
        "-D",
        "XNU_OWNER_BOOT_MANIFEST=1",
    ],
    # The source corecrypto provider is off, as in the 10063 tree: Apple's prelinked corecrypto kext is the only one,
    # and OSKext starts it like any other.
    "libkern/c++/OSKext.cpp": [
        "-U",
        "XNU_SOURCE_CORECRYPTO14",
    ],
    "osfmk/arm64/start.s": [
        "-D",
        "XNU_NATIVE_ARCH_TIMER=1",
        "-D",
        "SWITCH_XNU_BOARD=1",
    ],
    "osfmk/kern/startup.c": [
        "-D",
        "XNU_NATIVE_TRUST_CACHE=1",
    ],
    "osfmk/kern/zalloc.c": [
        "-D",
        "XNU_ZONE_PGZ_ALIGNMENT_PROBE=0",
    ],
    "osfmk/prng/prng_random.c": [
        "-U",
        "XNU_SOURCE_CORECRYPTO14",
    ],
    "pexpert/arm/pe_init.c": [
        "-D",
        "XNU_DT_PANIC_REGION=1",
    ],
}

# libpthread_kernel.a's XNU object (//kernel:libpthread), compiled as prng_random.c is.
PTHREAD_BOOTSTRAP = struct(
    objects = ["bsd/kern/source_pthread_bootstrap.c"],
    copts = {"bsd/kern/source_pthread_bootstrap.c": ["-U", "XNU_SOURCE_CORECRYPTO14", "-Wno-sign-conversion"]},
    includes = {},
    segments = {},
)

# This archive is linked after XNU's LTO pass and requires a machine-code bootstrap object.
PTHREAD_BOOTSTRAP_RELEASE = struct(
    objects = PTHREAD_BOOTSTRAP.objects,
    copts = {
        "bsd/kern/source_pthread_bootstrap.c": PTHREAD_BOOTSTRAP.copts["bsd/kern/source_pthread_bootstrap.c"] + ["-fno-lto"],
    },
    includes = {},
    segments = {},
)
