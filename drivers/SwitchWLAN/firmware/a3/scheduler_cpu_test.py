import unittest

from drivers.SwitchWLAN.firmware.a3 import control_cpu_test as switch_control


class SchedulerCPU(switch_control.ControlCPU):
    switch_image = 'scheduler_cpu'
    switch_cases = 39


if __name__ == '__main__':
    unittest.main()
