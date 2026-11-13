import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.kernel.libpthread import configuration_flags


class ConfigurationTest(unittest.TestCase):
    def test_development_preserves_existing_flags(self):
        self.assertEqual(configuration_flags('DEVELOPMENT'), ['-DMACH_ASSERT=1', '-DDEBUG=1'])

    def test_compiled_profiles_keep_errors_and_gate_diagnostics(self):
        source = """
#if DEBUG || DEVELOPMENT
int debug_observer(void) { return 1; }
#endif
#if MACH_ASSERT
int assertion_observer(void) { return 1; }
#endif
#if !IST_KDEBUG
int standard_trace_observer(void) { return 1; }
#endif
#if !CONFIG_NO_PRINTF_STRINGS
int printf_diagnostic(void) { return 1; }
#endif
#if !CONFIG_NO_KPRINTF_STRINGS
int kprintf_diagnostic(void) { return 1; }
#endif
int panic_error_path(void) { return 1; }
"""
        diagnostic_names = {
            '_debug_observer',
            '_assertion_observer',
            '_standard_trace_observer',
            '_printf_diagnostic',
            '_kprintf_diagnostic',
        }
        with tempfile.TemporaryDirectory() as scratch:
            for config in ('DEVELOPMENT', 'RELEASE'):
                with self.subTest(config=config):
                    output = Path(scratch) / (config + '.o')
                    subprocess.run(
                        [
                            '/usr/bin/xcrun',
                            'clang',
                            '-x',
                            'c',
                            '-c',
                            '-',
                            '-o',
                            str(output),
                            *configuration_flags(config),
                        ],
                        input=source,
                        text=True,
                        capture_output=True,
                        check=True,
                    )
                    symbols = subprocess.run(
                        ['/usr/bin/xcrun', 'nm', '-g', '-j', '-U', str(output)],
                        text=True,
                        capture_output=True,
                        check=True,
                    ).stdout.splitlines()
                    expected = {'_panic_error_path'}
                    if config == 'DEVELOPMENT':
                        expected |= diagnostic_names
                    self.assertEqual(set(symbols), expected)

    def test_unknown_configuration_is_rejected(self):
        with self.assertRaises(ValueError):
            configuration_flags('RELASE')


if __name__ == '__main__':
    unittest.main()
