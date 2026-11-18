import sys
from pathlib import Path

import icon_renders
from python.runfiles import runfiles


def main(root: Path, template: Path, *, render: bool) -> None:
    for bundle in (root / 'Payload/SwitchInstaller.app', root / 'SwitchInstaller.app', root):
        if bundle.name != 'SwitchInstaller.app' or not bundle.is_dir():
            continue
        profile = bundle / 'embedded.mobileprovision'
        if profile.is_file() and profile.read_bytes() == template.read_bytes():
            profile.unlink()
        if not render:
            paths = runfiles.Create()
            icon = Path(paths.Rlocation('_main/apps/installer/AppIcon.icon/icon.json')).parent
            inputs = Path(paths.Rlocation('_main/apps/installer/artwork/icon-renders.json')).parent
            icon_renders.apply(bundle, icon, inputs)
        return
    raise SystemExit('Installer bundle missing before signing')


if __name__ == '__main__':
    main(Path(sys.argv[1]), Path(runfiles.Create().Rlocation('_main/apps/adhoc.mobileprovision')), render=False)
