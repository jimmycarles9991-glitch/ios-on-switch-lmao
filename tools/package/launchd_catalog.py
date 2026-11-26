import argparse
import hashlib
import plistlib
import re
import subprocess
from pathlib import Path

from tools.trustcache.trustcache import trust_module

LABEL = 'com.brunostjohn.switch.bootstrap'
JOB_PATH = f'/System/Library/LaunchDaemons/{LABEL}.plist'
APPS_LABEL = 'com.brunostjohn.switch.bootstrap-apps'
APPS_JOB_PATH = f'/System/Library/LaunchDaemons/{APPS_LABEL}.plist'
SDWAKE_LABEL = 'com.brunostjohn.switch.sdwake'
SDWAKE_JOB_PATH = f'/System/Library/LaunchDaemons/{SDWAKE_LABEL}.plist'
SDWAKE_MATCH = {
    'com.apple.iokit.matching': {
        'com.brunostjohn.switch.sdwake.shared-volume': {
            'IOMatchLaunchStream': True,
            'IOProviderClass': 'IOMedia',
            'IOParentMatch': {
                'IOProviderClass': 'IOBlockStorageDevice',
                'IONameMatch': 'SwitchSDSharedVolume',
            },
        }
    }
}


def add_bootstrap_jobs(catalog: bytes, job: dict, apps_job: dict, sdwake_job: dict | None = None) -> bytes:
    if (
        job.get('Label') != LABEL
        or job.get('ProgramArguments') != ['/usr/libexec/switch-bootstrap']
        or job.get('UserName') != 'root'
        or job.get('RunAtLoad') is not True
    ):
        raise ValueError('invalid bootstrap LaunchDaemon')
    if (
        apps_job.get('Label') != APPS_LABEL
        or apps_job.get('ProgramArguments') != ['/usr/libexec/switch-appregd', '--bootstrap-service']
        or apps_job.get('UserName') != 'mobile'
        or apps_job.get('MachServices') != {APPS_LABEL: True}
        or apps_job['MachServices'][APPS_LABEL] is not True
        or apps_job.get('RunAtLoad', False) is not False
        or apps_job.get('KeepAlive', False) is not False
    ):
        raise ValueError('invalid bootstrap app registration LaunchDaemon')
    entries = [(JOB_PATH, job), (APPS_JOB_PATH, apps_job)]
    if sdwake_job is not None:
        if (
            sdwake_job.get('Label') != SDWAKE_LABEL
            or sdwake_job.get('ProgramArguments') != ['/usr/libexec/switch-sdwake']
            or sdwake_job.get('UserName') != 'root'
            or sdwake_job.get('LaunchEvents') != SDWAKE_MATCH
            or sdwake_job.get('RunAtLoad', False) is not False
            or sdwake_job.get('KeepAlive', False) is not False
        ):
            raise ValueError('invalid shared SD wake LaunchDaemon')
        entries.append((SDWAKE_JOB_PATH, sdwake_job))
    value = plistlib.loads(catalog, fmt=plistlib.FMT_BINARY)
    jobs = value['LaunchDaemons']
    if not isinstance(jobs, dict) or not jobs or any(not isinstance(existing, dict) for existing in jobs.values()):
        raise ValueError('invalid native launchd catalogue')
    for path, entry in entries:
        if path in jobs or any(existing.get('Label') == entry['Label'] for existing in jobs.values()):
            raise ValueError(f'{entry["Label"]} is already in the native catalogue')
        jobs[path] = entry
    return plistlib.dumps(value, fmt=plistlib.FMT_BINARY, sort_keys=False)


def read_pinned_catalog(catalog_path: Path, signature_path: Path, catalog_hash: str, signature_hash: str) -> bytes:
    catalog = catalog_path.read_bytes()
    signature = signature_path.read_bytes()
    if hashlib.sha256(catalog).hexdigest() != catalog_hash or hashlib.sha256(signature).hexdigest() != signature_hash:
        raise ValueError('native launchd catalogue or detached signature differs from the IPSW pins')
    return catalog


def sign_catalog(catalog: Path, signature: Path) -> bytes:
    codesign = '/usr/bin/codesign'
    subprocess.run(
        [
            codesign,
            '--sign',
            '-',
            '--identifier',
            'com.brunostjohn.switch.launchd-catalog',
            '--options',
            'runtime',
            '--timestamp=none',
            '--pagesize',
            '4096',
            '--digest-algorithm',
            'sha256',
            '--detached',
            str(signature),
            str(catalog),
        ],
        check=True,
        capture_output=True,
    )
    subprocess.run(
        [codesign, '--verify', '--strict', '--detached', str(signature), str(catalog)],
        check=True,
        capture_output=True,
    )
    info = subprocess.run(
        [codesign, '--display', '--verbose=5', '--detached', str(signature), str(catalog)],
        check=True,
        capture_output=True,
        text=True,
    ).stderr
    cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', info, re.MULTILINE)
    if (
        not cdhash
        or '\nFormat=generic\n' not in info
        or '\nSignature=adhoc\n' not in info
        or '\nHash type=sha256 size=32\n' not in info
        or '\nPage size=4096\n' not in info
    ):
        raise ValueError('launchd catalogue signature is not generic, ad hoc and SHA-256 with 4 KiB pages')
    return bytes.fromhex(cdhash[1])


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in (
        'native-catalog',
        'native-signature',
        'job',
        'apps-job',
        'sdwake-job',
        'catalog',
        'signature',
        'trust',
        'digest',
    ):
        parser.add_argument('--' + name, type=Path, required=True)
    for name in ('catalog-sha256', 'signature-sha256'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    job = plistlib.loads(args.job.read_bytes())
    apps_job = plistlib.loads(args.apps_job.read_bytes())
    sdwake_job = plistlib.loads(args.sdwake_job.read_bytes())
    catalog = read_pinned_catalog(
        args.native_catalog, args.native_signature, args.catalog_sha256, args.signature_sha256
    )
    args.catalog.parent.mkdir(parents=True, exist_ok=True)
    revised = add_bootstrap_jobs(catalog, job, apps_job, sdwake_job)
    args.catalog.write_bytes(revised)
    cdhash = sign_catalog(args.catalog, args.signature)
    args.digest.write_bytes(hashlib.sha256(revised).digest() + hashlib.sha256(args.signature.read_bytes()).digest())
    # The native catalogue's static trust entry has category 2 and no flags.
    args.trust.write_bytes(trust_module([(cdhash, 0, 2)]))


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        detail = error.stderr or error.stdout
        raise SystemExit(
            detail.decode(errors='replace') if isinstance(detail, bytes) else detail or str(error)
        ) from None
    except (KeyError, OSError, TypeError, ValueError) as error:
        raise SystemExit(str(error)) from None
