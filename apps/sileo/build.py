import argparse
import plistlib
import re
import shutil
import tempfile
from pathlib import Path

from apps.sileo import adapt
from bootstrap.xcode import build_environment, copy_source, strip_products, xcode


def copy_licenses(dependencies: dict, out: Path) -> None:
    for name, root in dependencies.items():
        found = []
        for path in root.rglob('*'):
            if path.is_file() and re.match(r'^(license|licence|copying|notice)([._-].*)?$', path.name, re.IGNORECASE):
                found.append(path)
                destination = out / 'licenses' / name / path.relative_to(root)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, destination)
        if not found:
            if name != 'depictionkit':
                raise ValueError(f'{name}: upstream source has no distribution license file')
            destination = out / 'licenses' / name
            destination.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root / 'README.md', destination / 'README.md')
            for path in (root / 'DepictionKit').rglob('*.swift'):
                relative = path.relative_to(root)
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)


def build(sileo: Path, dependencies: dict[str, Path], out: Path, xcode_env: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    copy_licenses(dependencies, out)
    with tempfile.TemporaryDirectory(prefix='switch-bootstrap-build-', dir=out.parent) as temporary:
        work = Path(temporary)
        env = build_environment(xcode_env, work)
        source = work / 'Sileo'
        copy_source(sileo, source)
        local = {}
        for name, path in dependencies.items():
            destination = work / 'dependencies' / name
            copy_source(path, destination)
            local[name] = destination
        depiction = source / 'Deps/DepictionKit'
        if depiction.exists():
            shutil.rmtree(depiction)
        copy_source(local.pop('depictionkit'), depiction)
        adapt.adapt_sileo(source)
        adapt.adapt_dependencies(local)
        adapt.localize_packages(source, local)
        settings = (
            'PRODUCT_BUNDLE_IDENTIFIER=org.coolstar.SileoStore',
            'DISPLAY_NAME=Sileo',
            'MARKETING_VERSION=2.5.1',
            'CURRENT_PROJECT_VERSION=2.5.1',
        )
        products = xcode(source, 'Sileo', 'Sileo', env, settings)
        shutil.copytree(products / 'Sileo.app', out / 'Sileo.app')
        shutil.copy2(source / 'Sileo/Entitlements.entitlements', out / 'Sileo.entitlements')
        shutil.copy2(sileo / 'LICENSE', out / 'Sileo.LICENSE')
        for info in (out / 'Sileo.app').rglob('Info.plist'):
            values = plistlib.loads(info.read_bytes())
            values.pop('BuildMachineOSBuild', None)
            info.write_bytes(plistlib.dumps(values, fmt=plistlib.FMT_BINARY, sort_keys=True))
        strip_products(out, work, env)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--dependency', action='append', default=[])
    parser.add_argument('--xcode-env', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    dependencies = {name: Path(path).resolve() for name, path in (v.split('=', 1) for v in args.dependency)}
    build(args.source.resolve(), dependencies, args.out.resolve(), args.xcode_env)


if __name__ == '__main__':
    main()
