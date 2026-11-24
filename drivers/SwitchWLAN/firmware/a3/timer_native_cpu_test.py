import unittest

from drivers.SwitchWLAN.firmware.a3 import control_cpu_test as switch_control


class SwitchNativeTimerCPU(switch_control.ControlCPU):
    switch_image = 'timer_native_cpu'
    switch_cases = 4


if __name__ == '__main__':
    unittest.main()
