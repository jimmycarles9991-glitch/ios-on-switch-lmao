import json
import plistlib
import re
import subprocess
import sys
from pathlib import Path

METHODS = {
    'dataClassName': '@16@0:8',
    'estimatedDuration': 'f16@0:8',
    'migrationProgress': 'f16@0:8',
    'performMigration': 'B16@0:8',
}
WRAPPER = 'com.apple.migrationpluginwrapper'


def scheduling(runner: Path) -> dict:
    disassembly = subprocess.check_output(['/usr/bin/xcrun', 'dyld_info', '-disassemble', runner], text=True)
    match = re.search(
        r'(?m)^-\[DMPluginsProvider initWithReps:categories:pluginAllowedList:\]:\n(.*?)(?=^[-+]\[)',
        disassembly,
        re.DOTALL,
    )
    if not match:
        return {'provider': 'unrecognized'}
    body = match[1]
    progress = re.search(r'(?m)^-\[DMClientInvocation isProgressHost\]:\n(.*?)(?=^[-+]\[)', disassembly, re.DOTALL)
    categories = [
        name
        for name in [
            'orderedUserAgnosticPluginSpecifiers',
            'orderedUserSpecificPluginSpecifiers',
            'concurrentUserSpecificPluginSpecifiers',
        ]
        if f'objc_msgSend${name}' in body
    ]
    return {
        'provider': 'DMPluginsProvider',
        'categories_from_allowed_list': categories,
        'identifier_lookups': body.count('objc_msgSend$dmps_identifier'),
        'scheduled_plugin_constructors': body.count('objc_msgSend$initWithIdentifier:fileSystemRep:isUserAgnostic:'),
        'serial_or_concurrent_array_additions': body.count('objc_msgSend$addObject:'),
        'progress_host_compares_backboardd': bool(progress and '@"com.apple.backboardd"' in progress[1]),
    }


def method_types(binary: Path, principal: str) -> dict:
    selectors = subprocess.check_output(['/usr/bin/xcrun', 'dyld_info', '-objc', binary], text=True)
    if f'@interface {principal} : DataClassMigrator' not in selectors:
        raise ValueError(f'{binary.name}: unexpected migrator superclass')
    # The donor strips method symbol names. dyld_info maps its implementation addresses back to selectors.
    addresses = {
        m: int(a, 16) for a, m in re.findall(rf'(0x[\da-fA-F]+)\s+-\[{re.escape(principal)} (\w+)\]', selectors)
    }
    objc = subprocess.check_output(['/usr/bin/otool', '-ov', binary], text=True)
    types = {int(a, 16): t for t, a in re.findall(r'types\s+[^\n]+ (\S+)\n\s+imp\s+\S+ \((0x[\da-fA-F]+)\)', objc)}
    observed = {name: types.get(addresses.get(name)) for name in METHODS}
    if observed != METHODS:
        raise ValueError(f'{binary.name}: {principal} method types {observed}')
    return observed


def report(root: Path) -> dict:
    library = root / 'System/Library'
    build = plistlib.loads((library / 'CoreServices/SystemVersion.plist').read_bytes())['ProductBuildVersion']
    plugins = library / 'DataClassMigrators'
    native = {
        plistlib.loads(p.read_bytes())['CFBundleIdentifier']: p.parent for p in plugins.glob('*.migrator/Info.plist')
    }
    dependencies = ['com.apple.-0LaunchServicesMigrator', 'com.apple.MobileInstallation.SystemAppMigrator']
    for bundle_id in dependencies:
        if bundle_id not in native:
            raise ValueError(f'{build}: missing migration prerequisite {bundle_id}')
    ls = native['com.apple.-0LaunchServicesMigrator']
    binary = ls / plistlib.loads((ls / 'Info.plist').read_bytes())['CFBundleExecutable']
    observed = method_types(binary, 'LSDatabaseMigrator')
    wrapper = library / 'PrivateFrameworks/DataMigration.framework/XPCServices' / f'{WRAPPER}.xpc' / WRAPPER
    signature = subprocess.check_output(['/usr/bin/codesign', '-dvv', wrapper], text=True, stderr=subprocess.STDOUT)
    if f'Identifier={WRAPPER}' not in signature.splitlines():
        raise ValueError(f'{build}: unexpected migration wrapper signature')
    runner = wrapper.parents[1] / 'com.apple.datamigrator.xpc/com.apple.datamigrator'
    schedule = scheduling(runner)
    if len(schedule.get('categories_from_allowed_list', [])) != 3 or not schedule.get(
        'progress_host_compares_backboardd'
    ):
        raise ValueError(f'{build}: unexpected migration scheduler or progress host: {schedule}')
    return {
        'build': build,
        'method_types': observed,
        'wrapper_identifier': WRAPPER,
        'dependencies': dependencies,
        'scheduling': schedule,
    }


def main(system_version: Path, pins_file: Path, out: Path) -> None:
    pins = json.loads(pins_file.read_bytes())
    root = system_version.parents[3]
    result = report(root)
    if result['build'] != pins['build']:
        raise ValueError(f'Expected {pins["build"]}, mounted {result["build"]}')
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main(*(Path(arg) for arg in sys.argv[1:]))
