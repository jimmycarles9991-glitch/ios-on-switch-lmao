"""libfirehose_kernel.a (and its installed header) from libdispatch.

Built with xcodebuild against the kernel SDK root, as the tree's scripts/build-firehose.sh did.

usage: firehose.py --xcode-env F --libdispatch DIR --xnu DIR --sdkroot DIR --out DIR --log F [--jobs N]
"""

import argparse
import os
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ('xcode-env', 'libdispatch', 'xnu', 'sdkroot', 'out', 'log', 'path-root'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--jobs', default='16')
    args = parser.parse_args()
    xcode = common.xcode_env(args.xcode_env)
    out = Path(args.out).absolute()
    sdkroot = Path(args.sdkroot).absolute()
    # The finished SDK root: the headers stage plus what this installs (as the CMake flow's one fakeroot).
    common.copy_tree(sdkroot, out)
    with tempfile.TemporaryDirectory() as scratch_dir, open(args.log, 'w') as log:
        scratch = Path(scratch_dir)
        # A writable libdispatch checkout next to "xnu-work" (its xcconfig names ../xnu-work/EXTERNAL_HEADERS).
        work = scratch / 'libdispatch-work'
        shutil.copytree(args.libdispatch, work, symlinks=False, copy_function=common.copy_writable)
        os.symlink(Path(args.xnu).absolute(), scratch / 'xnu-work')
        config = work / 'xcodeconfig/libfirehose_kernel.xcconfig'
        text = config.read_text()
        text = text.replace(
            '$(SDKROOT)/System/Library/Frameworks/Kernel.framework',
            '$(FAKEROOT_DIR)/System/Library/Frameworks/Kernel.framework',
        )
        text = text.replace('$(SDKROOT)/usr/local/include', '$(FAKEROOT_DIR)/usr/local/include')
        lines = text.splitlines()
        for index, line in enumerate(lines):
            if line.startswith('HEADER_SEARCH_PATHS =') and '$(SDKROOT)/usr/include' not in line:
                lines[index] += (
                    ' $(SDKROOT)/usr/include $(FAKEROOT_DIR)/usr/local/include'
                    ' $(PROJECT_DIR)/../xnu-work/EXTERNAL_HEADERS'
                )
        config.write_text('\n'.join(lines) + '\n')
        env = common.environment(xcode)
        cache = args.path_root.rstrip('/') + '/out/native-a57/root-v1/tree/.cache'
        # The temporary directory reaches clang under both its spellings: /private/var/... (the compiles' working
        # directory, resolved) and /var/... (the sources' paths, as xcodebuild normalizes them).
        real = os.path.realpath(scratch)
        temps = {real, real[len('/private') :] if real.startswith('/private/') else real, str(scratch)}
        places = [(t + '/libdispatch-work', cache + '/libdispatch-work') for t in sorted(temps)]
        places += [(t + '/xnu-work', cache + '/xnu-work') for t in sorted(temps)]
        places.append((os.path.realpath(out), cache + '/fakeroot'))
        maps = ' '.join(f'-ffile-prefix-map={path}={spelling}' for path, spelling in places)
        common.run(
            [
                'xcodebuild',
                'install',
                '-project',
                work / 'libdispatch.xcodeproj',
                '-target',
                'libfirehose_kernel',
                '-configuration',
                'Release',
                '-sdk',
                'iphoneos',
                '-jobs',
                args.jobs,
                'ARCHS=arm64',
                'VALID_ARCHS=arm64',
                'ONLY_ACTIVE_ARCH=NO',
                'IPHONEOS_DEPLOYMENT_TARGET=17.0',
                'CODE_SIGNING_ALLOWED=NO',
                'GCC_TREAT_WARNINGS_AS_ERRORS=NO',
                'OBJROOT=' + str(scratch / 'obj'),
                'SYMROOT=' + str(scratch / 'sym'),
                'DSTROOT=' + str(out),
                'FAKEROOT_DIR=' + str(out),
                # Where the CMake flow kept these trees, so no sandbox path reaches the objects.
                'OTHER_CFLAGS=$(inherited) ' + maps,
            ],
            env,
            cwd=scratch,
            log=log,
        )
        lib = out / 'usr/local/lib/kernel'
        if (lib / 'liblibfirehose_kernel.a').exists():
            shutil.copyfile(lib / 'liblibfirehose_kernel.a', lib / 'libfirehose_kernel.a')
        if not (lib / 'libfirehose_kernel.a').exists():
            sys.exit('xcodebuild did not install libfirehose_kernel.a')


if __name__ == '__main__':
    main()
