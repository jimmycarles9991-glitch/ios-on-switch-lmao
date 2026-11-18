import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

from bootstrap import upstream
from bootstrap.xcode import build_environment, copy_source, strip_products, xcode


def build(ellekit: Path, out: Path, xcode_env: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='switch-bootstrap-build-', dir=out.parent) as temporary:
        work = Path(temporary)
        env = build_environment(xcode_env, work)
        source = work / 'ElleKit'
        copy_source(ellekit, source)
        upstream.adapt_ellekit(source)
        for scheme in ('ellekit', 'injector', 'safemode'):
            products = xcode(source, 'ellekit', scheme, env)
        for source_name, destination in (
            ('libellekit.dylib', 'libellekit.dylib'),
            ('libinjector.dylib', 'libinjector.dylib'),
            ('libsafemode-ui.dylib', 'MobileSafety.dylib'),
        ):
            shutil.copy2(products / source_name, out / destination)
            install_name = (
                '/var/jb/usr/lib/' + destination
                if destination == 'libellekit.dylib'
                else '/var/jb/usr/lib/ellekit/' + destination
            )
            subprocess.run(
                [
                    '/usr/bin/install_name_tool',
                    '-id',
                    install_name,
                    '-change',
                    '/usr/local/lib/libellekit.dylib',
                    '/var/jb/usr/lib/libellekit.dylib',
                    str(out / destination),
                ],
                check=True,
                capture_output=True,
            )
        shutil.copy2(source / 'LICENSE', out / 'ElleKit.LICENSE')
        shutil.copy2(source / 'logger/Logger.swift', out / 'ElleKit.Logger.swift')
        strip_products(out, work, env)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--xcode-env', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    build(args.source.resolve(), args.out.resolve(), args.xcode_env)


if __name__ == '__main__':
    main()
