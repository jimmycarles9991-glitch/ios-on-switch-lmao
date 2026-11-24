import unittest

from drivers.SwitchWLAN.firmware.a3 import control_cpu_test as switch_control


class OwnedTimerCPU(switch_control.ControlCPU):
    switch_image = 'timer_owned_cpu'
    switch_cases = 13


if __name__ == '__main__':
    unittest.main()
