"""iPadOS 27's deploy updates to the pretranslated tree: the kernel's one line per update at capture.

kernel/switch/vfs/pretranslated_updates.c gives each file the boot package names either its update ("<path> from the
deploy's update") or no redirect at all ("<path> not updated (<why>); Apple's file runs translated at load time, without
this build's edit of it"). The second keeps the boot alive, but the file is Apple's own, translated at every exec and
without the edit the build needs (locationd then asks for the coprocessor services its capability edit takes out), so
any such line blocks the build from counting as good. So does a tree turned off: the kernel turns it off when the
/chosen list does not parse, since it could not tell which copies the boot package distrusts. "none applied" names why
the update folder could not be used; a missing folder alone is a fresh card's normal state, and the kernel prints it
only when an update was needed, which the per-file lines then fail. The update lines come at capture, so only a boot
whose capture line is in the log is judged, and a log with none is NO_DATA (24 Nov: every one of 22 iPadOS 27 boots
had it).
"""

from tools.devicerun.rules import REPORT, Check

OWNER = 'perf1-daemons'
SOURCE = 'kernel/switch/vfs/pretranslated_updates.c'
CAPTURE = 'pretranslated.capture'
OFF = 'SwitchPretranslated: off ('

PRETRANSLATED = [
    Check(
        CAPTURE,
        OWNER,
        (' files from /private/var/Switch/pretranslated (', OFF),
        REPORT,
        process='kernel',
        source='kernel/switch/vfs/pretranslated.c',
        note='the tree was captured, or turned off, at this boot',
    ),
    Check(
        'pretranslated.update',
        OWNER,
        'SwitchPretranslated: /',
        process='kernel',
        needs=CAPTURE,
        boots=CAPTURE,
        regex=r"^SwitchPretranslated: (?!.* from the deploy's update$)(?P<path>/\S+) ?(?P<result>.*)$",
        show=('path', 'result'),
        blocks=True,
        source=SOURCE,
        note="a file the boot package updates that did not get its update: Apple's file runs translated at load time, "
        "without the build's edit; the redirect's other per-file lines (a file that stays untranslated) fail here too",
    ),
    Check(
        'pretranslated.update-folder',
        OWNER,
        'updates needed, none applied (',
        process='kernel',
        needs=CAPTURE,
        boots=CAPTURE,
        regex=r'none applied \((?P<folder>[^:]+): (?!no such file\)$)(?P<reason>[^)]*)\)$',
        show=('folder', 'reason'),
        blocks=True,
        source=SOURCE,
        note="Data's update folder or its Switch/ parent could not be used (Switch/ must be root:wheel without group "
        'or other write; //deploy:sd sets it)',
    ),
    Check(
        'pretranslated.off',
        OWNER,
        OFF,
        process='kernel',
        regex=r'off \((?P<why>[^:]+): (?P<error>-?\d+)\)',
        show=('why', 'error'),
        blocks=True,
        source='kernel/switch/vfs/pretranslated.c',
        note="the tree is off and every System file runs translated at load time; '/chosen update list' is a boot "
        'package whose update list did not parse (tools/package/adt_edit.py)',
    ),
]
