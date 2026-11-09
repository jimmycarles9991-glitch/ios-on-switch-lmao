// SPDX-License-Identifier: BSD-2-Clause-Patent
// The thermal models of the IPSW kinds' iPads as SwitchSensors.h publishes them: one HID service per distinct matchName
// of the model's non-synth Sensors entries (thermalmonitord keys its readings by matchName, so entries that share one,
// the shadowSensor ones, share its service), each answering from the Switch source nearest to what the iPad's sensor
// measures. Plain C with no kernel dependency, for the host tests.
#ifndef SWITCH_SENSOR_MODELS_H
#define SWITCH_SENSOR_MODELS_H

// The J120AP-Info.plist Sensors array, non-synth entries, in order (index = thermalmonitord's sensor index and watchdog
// mask bit). 'i'/'a'/'x' = instantaneous / window average / window maximum on Apple's PMGR: each is one instantaneous
// reading here (thermalmonitord reads all three in the same cycle, so the one-sample average and maximum are that
// sample). Board thermistors (PMU tdevN, PMU2 tdevN, PMU tjunc) take the PCB channel: the one board-level point the
// Switch measures away from the cells. PMGR SOC die sensors take the die points outside the CPU cores (T210B01 has
// three: gpu, pllx, the TMP451 diode; the fourth slot is the GPU's, the largest block). ACC (the CPU complex) sensors
// 0-3 are SOC_THERM's per-core cpu0-cpu3.
// What thermalmonitord takes the PMU tcal channel (TP0Z) to read on a calibrated PMU: CommonProduct's
// calibrationResistorTempDefault, 51.82 C in both IPSWs (21H461 constant at 0x100062190, 24B119 at 0x10005f630). It adds
// that default minus its filtered TP0Z to every TP?d / TP?[A-Z] board thermistor, so TP0Z answering exactly the default
// leaves the Switch's PCB reading uncorrected. J120 seeds the filter with the default and drifts toward what TP0Z answers
// (6000-sample time constant); J181 averages the first five answers (requiredCalibrationResistorSamples).
#define SWITCH_SENSOR_TCAL_MILLIC_J120AP 51820
#define SWITCH_SENSOR_TCAL_MILLIC_J181AP 51820

#define SWITCH_SENSOR_COUNT_J120AP 38u
static const struct switch_sensor switch_sensors_j120ap[SWITCH_SENSOR_COUNT_J120AP] = {
    {"TG0B", "gas gauge battery", SWITCH_SRC_BATTERY, 0},
    {"TP1d", "PMU tdev1", SWITCH_SRC_TMP451_PCB, 1},
    {"TP2d", "PMU tdev2", SWITCH_SRC_TMP451_PCB, 0},
    {"TP3d", "PMU tdev3", SWITCH_SRC_TMP451_PCB, 1},
    {"TP4d", "PMU tdev4", SWITCH_SRC_TMP451_PCB, 1},
    {"TP5d", "PMU tdev5", SWITCH_SRC_TMP451_PCB, 1},
    {"TP6d", "PMU tdev6", SWITCH_SRC_TMP451_PCB, 0},
    {"TP7d", "PMU tdev7", SWITCH_SRC_TMP451_PCB, 1},
    {"TP8d", "PMU tdev8", SWITCH_SRC_TMP451_PCB, 0},
    {"TR1d", "PMU2 tdev1", SWITCH_SRC_TMP451_PCB, 1},
    {"TR2d", "PMU2 tdev2", SWITCH_SRC_TMP451_PCB, 1},
    {"TP0Z", "PMU tcal", SWITCH_SRC_TCAL, 0},
    {"TP0j", "PMU tjunc", SWITCH_SRC_TMP451_PCB, 1},
    {"TQ0B", "Charger tbat", SWITCH_SRC_BATTERY, 1},
    {"Th0i", "PMGR SOC Die Temp Sensor0", SWITCH_SRC_SOC_GPU, 1},
    {"Th0a", "Avg: PMGR SOC Die Temp Sensor0", SWITCH_SRC_SOC_GPU, 0},
    {"Th0x", "Max: PMGR SOC Die Temp Sensor0", SWITCH_SRC_SOC_GPU, 0},
    {"Th1i", "PMGR SOC Die Temp Sensor1", SWITCH_SRC_SOC_PLLX, 1},
    {"Th1a", "Avg: PMGR SOC Die Temp Sensor1", SWITCH_SRC_SOC_PLLX, 0},
    {"Th1x", "Max: PMGR SOC Die Temp Sensor1", SWITCH_SRC_SOC_PLLX, 0},
    {"Th2i", "PMGR SOC Die Temp Sensor2", SWITCH_SRC_TMP451_SOC, 1},
    {"Th2a", "Avg: PMGR SOC Die Temp Sensor2", SWITCH_SRC_TMP451_SOC, 0},
    {"Th2x", "Max: PMGR SOC Die Temp Sensor2", SWITCH_SRC_TMP451_SOC, 0},
    {"Th3i", "PMGR SOC Die Temp Sensor3", SWITCH_SRC_SOC_GPU, 1},
    {"Th3a", "Avg: PMGR SOC Die Temp Sensor3", SWITCH_SRC_SOC_GPU, 0},
    {"Th3x", "Max: PMGR SOC Die Temp Sensor3", SWITCH_SRC_SOC_GPU, 0},
    {"Tc0i", "ACC Temp Sensor0", SWITCH_SRC_SOC_CPU0, 1},
    {"Tc0a", "Avg: ACC Temp Sensor0", SWITCH_SRC_SOC_CPU0, 0},
    {"Tc0x", "Max: ACC Temp Sensor0", SWITCH_SRC_SOC_CPU0, 0},
    {"Tc1i", "ACC Temp Sensor1", SWITCH_SRC_SOC_CPU1, 1},
    {"Tc1a", "Avg: ACC Temp Sensor1", SWITCH_SRC_SOC_CPU1, 0},
    {"Tc1x", "Max: ACC Temp Sensor1", SWITCH_SRC_SOC_CPU1, 0},
    {"Tc2i", "ACC Temp Sensor2", SWITCH_SRC_SOC_CPU2, 1},
    {"Tc2a", "Avg: ACC Temp Sensor2", SWITCH_SRC_SOC_CPU2, 0},
    {"Tc2x", "Max: ACC Temp Sensor2", SWITCH_SRC_SOC_CPU2, 0},
    {"Tc3i", "ACC Temp Sensor3", SWITCH_SRC_SOC_CPU3, 1},
    {"Tc3a", "Avg: ACC Temp Sensor3", SWITCH_SRC_SOC_CPU3, 0},
    {"Tc3x", "Max: ACC Temp Sensor3", SWITCH_SRC_SOC_CPU3, 0},
};

// J181AP-Info.plist (24B119, iOS 27) Sensors: 47 non-synth entries, 13 of them shadowSensor ("Th0f" and the like: the
// "Avg:" reading again, filtered on its own), so 34 services. thermalmonitord counts them against its matchNames less its
// shadows; every entry, shadows included, has a watchdog bit (0x7fffffffffff). Board thermistors (TP?d) and NAND CH0
// take the PCB channel; TG0B and TG0V the battery thermistor. PMGR SOC die sensors 0-2 are J120's points (gpu, pllx, the
// TMP451 diode). The A13's clusters map onto the one A57 cluster: the P cluster's sensors on cpu0 and cpu1 (pACC Temp
// Sensor n on cpu n, its MTR sensors 1 and 4 on cpu0 and cpu1), the E cluster's on cpu2 and cpu3 (eACC Temp Sensor0 on
// cpu2, SOC MTR Sensor3, whose maximum guards the E cores' lifetime, on cpu3). SOC MTR Sensor0 guards the GPU's and
// takes the gpu tsensor, as do GPU MTR 2 and 3; the Switch has no ANE, whose MTR sensor takes the SoC diode.
#define SWITCH_SENSOR_COUNT_J181AP 34u
static const struct switch_sensor switch_sensors_j181ap[SWITCH_SENSOR_COUNT_J181AP] = {
    {"TG0B", "TG0B", SWITCH_SRC_BATTERY, 0},
    {"TG0V", "TG0V", SWITCH_SRC_BATTERY, 1},
    {"TP1A", "TP1d", SWITCH_SRC_TMP451_PCB, 1},
    {"TP2C", "TP2d", SWITCH_SRC_TMP451_PCB, 0},
    {"TP3R", "TP3d", SWITCH_SRC_TMP451_PCB, 0},
    {"TP4H", "TP4d", SWITCH_SRC_TMP451_PCB, 1},
    {"TP5d", "TP5d", SWITCH_SRC_TMP451_PCB, 0},
    {"TP0Z", "PMU tcal", SWITCH_SRC_TCAL, 0},
    {"Th0a", "Avg: PMGR SOC Die Temp Sensor0", SWITCH_SRC_SOC_GPU, 0},
    {"Th0x", "Max: PMGR SOC Die Temp Sensor0", SWITCH_SRC_SOC_GPU, 0},
    {"Th1a", "Avg: PMGR SOC Die Temp Sensor1", SWITCH_SRC_SOC_PLLX, 0},
    {"Th1x", "Max: PMGR SOC Die Temp Sensor1", SWITCH_SRC_SOC_PLLX, 0},
    {"Th2a", "Avg: PMGR SOC Die Temp Sensor2", SWITCH_SRC_TMP451_SOC, 0},
    {"Th2x", "Max: PMGR SOC Die Temp Sensor2", SWITCH_SRC_TMP451_SOC, 0},
    {"Tc0a", "Avg: pACC Temp Sensor0", SWITCH_SRC_SOC_CPU0, 0},
    {"Tc0x", "Max: pACC Temp Sensor0", SWITCH_SRC_SOC_CPU0, 0},
    {"Tc1a", "Avg: pACC Temp Sensor1", SWITCH_SRC_SOC_CPU1, 0},
    {"Tc1x", "Max: pACC Temp Sensor1", SWITCH_SRC_SOC_CPU1, 0},
    {"Tc2a", "Avg: eACC Temp Sensor0", SWITCH_SRC_SOC_CPU2, 0},
    {"Tc2x", "Max: eACC Temp Sensor0", SWITCH_SRC_SOC_CPU2, 0},
    {"Ts0a", "Avg: SOC MTR Temp Sensor0", SWITCH_SRC_SOC_GPU, 0},
    {"Ts0x", "Max: SOC MTR Temp Sensor0", SWITCH_SRC_SOC_GPU, 0},
    {"Ts3a", "Avg: SOC MTR Temp Sensor3", SWITCH_SRC_SOC_CPU3, 0},
    {"Ts3x", "Max: SOC MTR Temp Sensor3", SWITCH_SRC_SOC_CPU3, 0},
    {"Ta0a", "Avg: ANE MTR Temp Sensor0", SWITCH_SRC_TMP451_SOC, 0},
    {"Ta0x", "Max: ANE MTR Temp Sensor0", SWITCH_SRC_TMP451_SOC, 0},
    {"Tp1a", "Avg: pACC MTR Temp Sensor1", SWITCH_SRC_SOC_CPU0, 0},
    {"Tp1x", "Max: pACC MTR Temp Sensor1", SWITCH_SRC_SOC_CPU0, 0},
    {"Tp4a", "Avg: pACC MTR Temp Sensor4", SWITCH_SRC_SOC_CPU1, 0},
    {"Tp4x", "Max: pACC MTR Temp Sensor4", SWITCH_SRC_SOC_CPU1, 0},
    {"Tg2x", "Max: GPU MTR Temp Sensor2", SWITCH_SRC_SOC_GPU, 0},
    {"Tg2a", "Avg: GPU MTR Temp Sensor2", SWITCH_SRC_SOC_GPU, 0},
    {"Tg3a", "Avg: GPU MTR Temp Sensor3", SWITCH_SRC_SOC_GPU, 0},
    {"TN0n", "NAND CH0 temp", SWITCH_SRC_TMP451_PCB, 1},
};

#endif
