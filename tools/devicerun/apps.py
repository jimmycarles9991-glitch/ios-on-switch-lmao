"""The overlay's apps in /Applications stay registered with LaunchServices, and changing them runs no migration.

switch-appregd registers a changed app at the next boot (tools/deploy/app_journal.py). A data migration runs only when
the kernel logs that one was asked for, which a deploy does only for a restage or --rerun-migration, and an install's
first boot does.

lsd's first-boot work (_LSServerCleanEphemeralMobileContainersForFirstBoot, 21H461 and 24B119) removes, after each
reboot, every container that is not the root, system data, Preboot or user data volume, with the bundles filed under it.
Until the kernel named the root volume for the overlay's /Applications union (kernel/switch/vfs/identity.c), the
overlay's apps were filed under the union's own volume: SwitchHWTest disappeared on every boot after the one that
registered it (boot 16b: 267 apps against 269, SpringBoard's recents naming it 'not installed').
"""

from tools.devicerun.rules import REPORT, Check

OWNER = 'r123-appreg'
REGISTRATION_OWNER = 'r129-appreg2'
LSD = 'lsd'
FIRST_BOOT = 'apps.lsd-first-boot'
CLEANUP = 'CoreServices _LSServerCleanEphemeralMobileContainersForFirstBoot'
MIGRATION_REQUESTED = 'apps.migration-requested'
MIGRATION_CHECKED = 'apps.migration-checked'
BACKBOARD = 'BackBoardServices data migration (backboardd)'
REGISTRAR = 'switch-appregd'

APPS = [
    Check(
        FIRST_BOOT,
        OWNER,
        'Reboot detected, performing first boot work.',
        'boots >= 1',
        process=LSD,
        source=CLEANUP,
    ),
    Check(
        'apps.overlay-container-removed',
        OWNER,
        'Marking container at /Applications/ for removal',
        process=LSD,
        needs=FIRST_BOOT,
        source=CLEANUP,
        note="the overlay's apps were filed under the union's own volume; one line on the first boot after a store "
        'that predates the fix is that store',
    ),
    Check(
        'apps.overlay-not-installed',
        OWNER,
        'one of the referenced apps is not installed',
        process='SpringBoard',
        regex=r'primary: (?P<app>com\.brunostjohn\.switch\.[\w.]+)',
        show=('app',),
        source='SpringBoard.framework -[SBRecentAppLayouts _validateAndUpdateRecents:]',
        note='its "signature version was no longer supported" line is the same nil app',
    ),
    Check(
        MIGRATION_REQUESTED,
        REGISTRATION_OWNER,
        'SwitchRawBoot: data migration requested',
        REPORT,
        process='kernel',
        source='kern_exec.c native_switch_system_app_migration',
        note="the deploy's sentinel or an install's first boot",
    ),
    Check(
        MIGRATION_CHECKED,
        REGISTRATION_OWNER,
        'Migration complete (if performed)',
        REPORT,
        process='backboardd',
        regex=r'Elapsed time: (?P<seconds>[\d.]+) seconds',
        show=('seconds',),
        source=BACKBOARD,
    ),
    Check(
        'apps.unrequested-migration',
        REGISTRATION_OWNER,
        'Data migrator completed with success',
        process='backboardd',
        needs=MIGRATION_CHECKED,
        unless=MIGRATION_REQUESTED,
        source=BACKBOARD,
        note='a migration in a boot the kernel did not ask for one: a deploy that changes an app must not run one',
    ),
    Check(
        'apps.registered',
        REGISTRATION_OWNER,
        'SwitchAppRegistration: ',
        REPORT,
        process=REGISTRAR,
        regex=r'^SwitchAppRegistration: (?P<stage>Register|Unregister) (?P<app>\S+) ',
        show=('stage', 'app'),
        source='drivers/SwitchAppRegistration/appregd.mm',
    ),
    Check(
        'apps.registration-failed',
        REGISTRATION_OWNER,
        'SwitchAppRegistration: ',
        process=REGISTRAR,
        level='Error',
        source='drivers/SwitchAppRegistration/appregd.mm',
    ),
]
