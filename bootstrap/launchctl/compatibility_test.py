import io
import os
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

from bootstrap import packages
from bootstrap.launchctl import adapt, package


class LaunchctlTests(unittest.TestCase):
    def test_bootstrap_preserves_native_routing_and_classifies_single_job_reply(self):
        upstream = next(Path(os.environ['TEST_SRCDIR']).glob('*bootstrap_launchctl/bootstrap.c'))
        source = adapt.adapt_bootstrap(upstream.read_text())
        body = '\n'.join(line for line in source.splitlines() if not line.startswith('#include '))
        header = Path(os.environ['TEST_SRCDIR']) / '_main/bootstrap/launchctl/daemon_result.h'
        decoder = header.with_suffix('.c')
        harness = """
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <xpc/xpc.h>
#include "daemon_result.h"
#define EUSAGE 117
#define EBADNAME 140
#define ENODOMAIN 112
#define XPC_ROUTINE_LOAD 800
#define XPC_ROUTINE_UNLOAD 801
static bool switch_job_system_session;
static unsigned switch_expected_domain = 2;
static int switch_native_result;
static xpc_object_t switch_native_reply;
int launchctl_setup_xpc_dict_for_service_name(const char *name, xpc_object_t dict, const char **service) {
    assert(!strcmp(name, "system"));
    *service = NULL;
    xpc_dictionary_set_uint64(dict, "type", 1);
    return 0;
}
xpc_object_t launchctl_parse_load_unload(unsigned domain, int count, char **paths) {
    assert(!domain && count == 1);
    assert(!strcmp(paths[0], "/var/jb/Library/LaunchDaemons/job.plist"));
    xpc_object_t array = xpc_array_create(NULL, 0);
    xpc_array_set_string(array, XPC_ARRAY_APPEND, paths[0]);
    return array;
}
int xpc_user_sessions_enabled(void) { return 1; }
unsigned xpc_user_sessions_get_foreground_uid(unsigned flags) { assert(!flags); return 501; }
xpc_object_t launchctl_xpc_from_plist(const char *path) {
    assert(strstr(path, "job.plist"));
    xpc_object_t job = xpc_dictionary_create(NULL, NULL, 0);
    if (switch_job_system_session)
        xpc_dictionary_set_string(job, "LimitLoadToSessionType", "System");
    return job;
}
int launchctl_send_xpc_to_launchd(int routine, xpc_object_t message, xpc_object_t *reply) {
    assert(routine == XPC_ROUTINE_LOAD || routine == XPC_ROUTINE_UNLOAD);
    assert(xpc_dictionary_get_uint64(message, "type") == switch_expected_domain);
    if (switch_expected_domain == 2)
        assert(xpc_dictionary_get_uint64(message, "handle") == 501);
    *reply = switch_native_reply;
    return switch_native_result;
}
#define xpc_strerror strerror
"""
        main = """
int main(void) {
    char *argv[] = {"bootstrap", "--switch-bootstrap-result", "system",
                   "/var/jb/Library/LaunchDaemons/job.plist", NULL};
    switch_native_reply = xpc_dictionary_create(NULL, NULL, 0);
    xpc_object_t message = NULL;
    assert(bootstrap_cmd(&message, 4, argv, NULL, NULL) == SWITCH_LAUNCHCTL_CHANGED);
    switch_job_system_session = true;
    switch_expected_domain = 1;
    xpc_dictionary_set_int64(switch_native_reply, "bootstrap-error", 119);
    assert(bootstrap_cmd(&message, 4, argv, NULL, NULL) == SWITCH_LAUNCHCTL_DISABLED);
    xpc_dictionary_set_int64(switch_native_reply, "bootstrap-error", EEXIST);
    assert(bootstrap_cmd(&message, 4, argv, NULL, NULL) == SWITCH_LAUNCHCTL_PRESENT);
    xpc_dictionary_set_int64(switch_native_reply, "bootstrap-error", EACCES);
    assert(bootstrap_cmd(&message, 4, argv, NULL, NULL) == SWITCH_LAUNCHCTL_FAILED);
    switch_native_result = 113;
    assert(bootout_cmd(&message, 4, argv, NULL, NULL) == SWITCH_LAUNCHCTL_ABSENT);
    switch_native_result = 0;
    xpc_object_t errors = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_int64(errors, "job.plist", 119);
    xpc_dictionary_set_value(switch_native_reply, "errors", errors);
    xpc_dictionary_set_int64(switch_native_reply, "bootstrap-error", 119);
    char *ordinary[] = {"bootstrap", "system", "/var/jb/Library/LaunchDaemons/job.plist", NULL};
    assert(bootstrap_cmd(&message, 3, ordinary, NULL, NULL) == 119);
    assert(bootstrap_cmd(&message, 3, argv, NULL, NULL) == EUSAGE);
}
"""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            program, executable = root / 'test.c', root / 'test'
            program.write_text(harness + body + main)
            subprocess.run(
                [
                    '/usr/bin/xcrun',
                    '--sdk',
                    'macosx',
                    'clang',
                    '-fblocks',
                    '-I',
                    str(header.parent),
                    str(program),
                    str(decoder),
                    '-o',
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True, capture_output=True)

    def test_userswitch_when_legacy_symbol_is_present_or_absent(self):
        upstream = next(Path(os.environ['TEST_SRCDIR']).glob('*bootstrap_launchctl/userswitch.c'))
        source = adapt.adapt_userswitch(upstream.read_text())
        body = source[source.index('\nint\nuserswitch_cmd(') :]
        harness = """
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *xpc_object_t;
#define EUSAGE 117
static bool available;
static int calls;
static int legacy_switch(long old_uid, long new_uid) {
    assert(old_uid == 501 && new_uid == 502);
    ++calls;
    return 0;
}
static void *native_lookup(void *scope, const char *symbol) {
    assert(scope == RTLD_DEFAULT && !strcmp(symbol, "launch_active_user_switch"));
    return available ? (void *)legacy_switch : NULL;
}
#define dlsym native_lookup
#define xpc_strerror strerror
"""
        main = """
int main(void) {
    char *argv[] = {"userswitch", "501", "502", NULL};
    assert(userswitch_cmd(NULL, 3, argv, NULL, NULL) == ENOTSUP);
    assert(!calls);
    available = true;
    assert(userswitch_cmd(NULL, 3, argv, NULL, NULL) == 0);
    assert(calls == 1);
    assert(userswitch_cmd(NULL, 2, argv, NULL, NULL) == EUSAGE);
    assert(calls == 1);
}
"""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            program, executable = root / 'test.c', root / 'test'
            program.write_text(harness + body + main)
            subprocess.run(
                ['/usr/bin/xcrun', '--sdk', 'macosx', 'clang', str(program), '-o', str(executable)], check=True
            )
            subprocess.run([str(executable)], check=True, capture_output=True)

    def test_replacement_preserves_package_identity_alias_and_license(self):
        status = 'Library/dpkg/status'
        entries = {}
        packages.add_file(
            entries,
            status,
            b'Package: launchctl\nArchitecture: iphoneos-arm64\nVersion: 1:1.1.1\nEssential: yes\n'
            b'Status: install ok installed\n\nPackage: tweak\nVersion: 1.2\nStatus: install ok installed\n',
        )
        license_path = 'usr/share/doc/launchctl/LICENSE'
        packages.add_file(entries, 'usr/bin/launchctl', b'old')
        packages.add_link(entries, 'bin/launchctl', '/var/jb/usr/bin/launchctl')
        packages.add_file(entries, license_path, b'previous notice')
        packages.add_file(
            entries,
            'Library/dpkg/info/launchctl.list',
            (
                '/.\n/var/jb/usr/bin/launchctl\n/var/jb/bin/launchctl -> /var/jb/usr/bin/launchctl\n'
                f'/var/jb/{license_path}\n'
            ).encode(),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            new, deb, notice = root / 'launchctl', root / 'launchctl.deb', root / 'LICENSE'
            new.write_bytes(b'new')
            notice.write_bytes(b'current upstream notice')
            package.adapt_launchctl(entries, deb, new, notice)
            result = packages.read_deb(deb)
        with tarfile.open(fileobj=io.BytesIO(result['data.tar.gz'])) as archive:
            executable = archive.getmember('var/jb/usr/bin/launchctl')
            self.assertEqual((executable.mode, executable.uid, executable.gid), (0o755, 0, 0))
            self.assertEqual(archive.extractfile(executable).read(), b'new')
            self.assertEqual(archive.getmember('var/jb/bin/launchctl').linkname, '/var/jb/usr/bin/launchctl')
            self.assertEqual(archive.extractfile(f'var/jb/{license_path}').read(), b'current upstream notice')
        self.assertIn(f'Version: {package.VERSION}\nStatus: hold ok installed'.encode(), entries[status][1])
        self.assertIn(b'Package: tweak\nVersion: 1.2\nStatus: install ok installed', entries[status][1])


if __name__ == '__main__':
    unittest.main()
