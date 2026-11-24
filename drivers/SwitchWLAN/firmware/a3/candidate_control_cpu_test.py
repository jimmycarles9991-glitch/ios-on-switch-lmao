import unittest

from drivers.SwitchWLAN.firmware.a3 import control_cpu_test as switch_control


class CandidateControlCPU(switch_control.ControlCPU):
    switch_image = 'candidate_control_cpu'
    switch_cases = 39


class SwitchCandidateControlLTOCPU(CandidateControlCPU):
    switch_image = 'candidate_control_lto_cpu'


if __name__ == '__main__':
    unittest.main()
