"""Run a NAK unit-test binary built in NVK's configuration (mesa_files), passing it the test filter.

usage: run_host_test.py <binary> <filter>
"""

import subprocess
import sys

sys.exit(subprocess.run([sys.argv[1], sys.argv[2]], check=False).returncode)
