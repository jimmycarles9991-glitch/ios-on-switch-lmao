import os
import plistlib
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from bootstrap import upstream


def build_injector_test(root: Path, program: str) -> str:
    (root / 'filter.c').write_text(program)
    executable = str(root / 'filter')
    subprocess.run(
        [
            '/usr/bin/xcrun',
            '--sdk',
            'macosx',
            'clang',
            '-Wall',
            '-Wextra',
            '-Werror',
            str(root / 'filter.c'),
            '-framework',
            'CoreFoundation',
            '-lobjc',
            '-o',
            executable,
        ],
        check=True,
        capture_output=True,
    )
    return executable


class UpstreamTests(unittest.TestCase):
    def test_actual_branch_source_adaptation(self):
        source = next(Path(os.environ['TEST_SRCDIR']).glob('*bootstrap_ellekit/ellekit'))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for path in (
                'Assembler/Branches.swift',
                'Assembler/Rebind.swift',
                'Languages/C/Hook.swift',
                'Languages/C/Orig.swift',
            ):
                destination = root / path
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source / path, destination)
            upstream.adapt_ellekit_branches(root)

            branches = (root / 'Assembler/Branches.swift').read_text()
            self.assertIn('Int(signExtend(UInt32(opcode & 0x3FFFFFF), 25)) * 4', branches)
            self.assertIn('pc &+ UInt64(bitPattern: Int64(disassembleBranchImm(opcode)))', branches)
            self.assertIn('branchDestination(reverse(isn), pc: UInt64(UInt(bitPattern: target)))', branches)
            self.assertIn('(-0x08000000 ..< 0x08000000).contains(offset) && offset.isMultiple(of: 4)', branches)

            rebind = (root / 'Assembler/Rebind.swift').read_text()
            self.assertIn('branchDestination(UInt64(reversed), pc: formerPC + UInt64(4 * index))', rebind)
            for link in ('true', 'false'):
                self.assertIn(f'assembleJump(destination, pc: newPC, link: {link}', rebind)
            self.assertIn('if checkBranchUncond(isn) {', (root / 'Languages/C/Orig.swift').read_text())

            hook = (root / 'Languages/C/Hook.swift').read_text()
            self.assertIn('targetSize >= 4 && !encodableBranchOffset(branchOffset)', hook)
            guard = hook.index('guard encodableBranchOffset(branchOffset)')
            branch = hook.index('b(branchOffset / 4)', guard)
            self.assertIn('hooks.removeValue(forKey: target)\n            return nil', hook[guard:branch])

    def test_actual_injector_filters(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            injector = root / 'injector.c'
            injector.write_bytes(
                next(Path(os.environ['TEST_SRCDIR']).glob('*bootstrap_ellekit/injector/injector.c')).read_bytes()
            )
            upstream.adapt_ellekit_filters(injector)
            text = injector.read_text()
            program = """
#include <CoreFoundation/CoreFoundation.h>
#include <assert.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int allocations_until_failure = -1;
static void *switch_malloc(size_t size) {
    if (allocations_until_failure == 0) return NULL;
    if (allocations_until_failure > 0) allocations_until_failure--;
    return malloc(size);
}
static const char *switch_string_ptr(CFStringRef value, CFStringEncoding encoding) {
    return allocations_until_failure == 0 ? NULL : CFStringGetCStringPtr(value, encoding);
}
#define malloc switch_malloc
#define CFStringGetCStringPtr switch_string_ptr
"""
            program += text[text.index('char* append_str(') : text.index('\nint alphasort2')]
            program += r"""
int main(int argc, char **argv) {
    if (argc == 1) {
        printf("%.17g\n", kCFCoreFoundationVersionNumber);
        return 0;
    }
    assert(argc == 3);
    allocations_until_failure = atoi(argv[2]);
    bool manager = false;
    bool matches = tweak_needinject(argv[1], &manager);
    return matches + 2 * manager;
}
"""
            executable = build_injector_test(root, program)
            current = float(subprocess.check_output([executable], text=True))
            for value, expected in (
                ([], 0),
                ({}, 0),
                ({'Filter': []}, 0),
                ({'Filter': {}, 'IsTweakManager': True}, 0),
                ({'Filter': {'Bundles': 'scalar', 'Classes': 5, 'Executables': 'filter'}}, 0),
                ({'Filter': {'Bundles': [1], 'Classes': [False], 'Executables': [2, 'é']}}, 0),
                ({'Filter': {'Bundles': 1, 'Classes': [False], 'Executables': [2, 'filter']}}, 1),
                ({'Filter': {'Executables': ['filter']}, 'IsTweakManager': 'true'}, 1),
                ({'Filter': {'Executables': ['filter']}, 'IsTweakManager': True}, 3),
                ({'Filter': {'Executables': ['filter'], 'CoreFoundationVersion': [current - 1, current + 1]}}, 1),
                ({'Filter': {'Executables': ['filter'], 'CoreFoundationVersion': [current]}}, 1),
                ({'Filter': {'Executables': ['filter'], 'CoreFoundationVersion': 0}}, 0),
                ({'Filter': {'Executables': ['filter'], 'CoreFoundationVersion': [0, 'high']}}, 0),
                ({'Filter': {'Executables': ['filter'], 'CoreFoundationVersion': [float('nan')]}}, 0),
                ({'Filter': {'Executables': ['filter'], 'CoreFoundationVersion': [current + 1]}}, 0),
                ({'Filter': {'Executables': ['filter'], 'CoreFoundationVersion': [0, current]}}, 0),
            ):
                with self.subTest(value=value):
                    (root / 'tweak.plist').write_bytes(plistlib.dumps(value, fmt=plistlib.FMT_BINARY))
                    test = subprocess.run([executable, str(root / 'tweak'), '-1'], capture_output=True, check=False)
                    self.assertEqual(test.returncode, expected, test.stderr)
            (root / 'tweak.plist').write_bytes(plistlib.dumps({'Filter': {'Executables': ['filter']}}))
            for allocations in ('0', '1'):
                test = subprocess.run([executable, str(root / 'tweak'), allocations], capture_output=True, check=False)
                self.assertEqual(test.returncode, 0, test.stderr)

    def test_actual_mobile_safety_startup(self):
        pin = next(Path(os.environ['TEST_SRCDIR']).glob('*bootstrap_ellekit'))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            injector, ui = root / 'injector.c', root / 'tweak.swift'
            shutil.copyfile(pin / 'injector/injector.c', injector)
            shutil.copyfile(pin / 'sb/tweak.swift', ui)
            upstream.adapt_ellekit_safety(injector, ui)
            text = injector.read_text()
            program = r"""
#include <CoreFoundation/CoreFoundation.h>
#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#undef TARGET_OS_OSX
#define TARGET_OS_OSX 0
static bool rootless, springboard, marker, deny_probe, deny_unlink, deny_signal, load_fails;
static int loaded, injected, reraised;
static void (*crash_handler)(int);
static CFBundleRef switch_bundle(void) { return (CFBundleRef)CFSTR("main"); }
static CFStringRef switch_identifier(CFBundleRef bundle) {
    assert(bundle);
    return springboard ? CFSTR("com.apple.springboard") : CFSTR("com.example.app");
}
static int switch_access(const char *path, int mode) {
    assert(mode == F_OK);
    return !strcmp(path, "/var/jb/usr/lib/ellekit/libinjector.dylib") || marker ? 0 : -1;
}
static int switch_open(const char *path, int flags, ...) {
    assert(!strcmp(path, "/var/mobile/.eksafemode"));
    assert(flags & O_NOFOLLOW);
    if ((flags & O_EXCL) && (deny_probe || marker)) return -1;
    marker = true;
    return 7;
}
static int switch_close(int fd) { assert(fd == 7); return 0; }
static int switch_unlink(const char *path) {
    assert(!strcmp(path, "/var/mobile/.eksafemode") && marker);
    if (deny_unlink) return -1;
    marker = false;
    return 0;
}
static void *switch_dlopen(const char *path, int flags) {
    assert(flags == RTLD_NOW && !strcmp(path, "/var/jb/usr/lib/ellekit/MobileSafety.dylib"));
    loaded++;
    return load_fails ? NULL : &loaded;
}
static char *switch_dlerror(void) { return "test error"; }
static int switch_sigaction(int number, const struct sigaction *action, struct sigaction *previous) {
    assert(number != 0 && !previous && action->sa_handler);
    if (deny_signal) return -1;
    crash_handler = action->sa_handler;
    return 0;
}
static void switch_signal(int number, void (*handler)(int)) { assert(number && handler == SIG_DFL); }
static int switch_raise(int number) { reraised = number; return 0; }
static void sandbox_extension_consume(const char *extension) { assert(!extension); }
static void tweaks_iterate(void) { injected++; }
static void switch_log(const char *message, ...) { assert(message); }

#define CFBundleGetMainBundle switch_bundle
#define CFBundleGetIdentifier switch_identifier
#define access switch_access
#define open switch_open
#define close switch_close
#define unlink switch_unlink
#define dlopen switch_dlopen
#define dlerror switch_dlerror
#define sigaction(...) switch_sigaction(__VA_ARGS__)
#define signal switch_signal
#define raise switch_raise
#define os_log_error(log, ...) switch_log(__VA_ARGS__)
"""
            program += '\n'.join(line for line in text.splitlines() if line.startswith('#define MOBILESAFETY_PATH_'))
            safety = text[text.index('static const char switch_safety_marker[]') :]
            program += '\n#if !TARGET_OS_OSX\n' + safety.replace('__attribute__((constructor)) ', '')
            program += """
int main(void) {
    unsetenv("_MSSafeMode");
    unsetenv("_SafeMode");
    unsetenv("SANDBOX_EXTENSION");
    injection_init();
    assert(injected == 1 && !loaded && !crash_handler);
    springboard = load_fails = true;
    injection_init();
    assert(injected == 1 && loaded == 1 && !marker);
    load_fails = false;
    marker = true;
    injection_init();
    assert(injected == 1 && loaded == 2 && marker && !crash_handler);
    marker = false;
    deny_probe = true;
    injection_init();
    assert(injected == 1 && !marker && !crash_handler);
    deny_probe = false;
    deny_unlink = true;
    injection_init();
    assert(injected == 1 && marker && !crash_handler);
    marker = deny_unlink = false;
    deny_signal = true;
    injection_init();
    assert(injected == 1 && !marker && !crash_handler);
    deny_signal = false;
    injection_init();
    assert(injected == 2 && !marker && crash_handler);
    crash_handler(SIGABRT);
    assert(marker && reraised == SIGABRT);
    return 0;
}
"""
            subprocess.run([build_injector_test(root, program)], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
