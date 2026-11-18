import sys
from pathlib import Path

from prepare_bundle import main
from python.runfiles import runfiles

if __name__ == '__main__':
    main(Path(sys.argv[1]), Path(runfiles.Create().Rlocation('_main/apps/adhoc.mobileprovision')), render=True)
