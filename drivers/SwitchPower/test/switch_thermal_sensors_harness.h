// switch_thermal_sensors_harness.c's exported wrappers of drivers/SwitchPower/SwitchSensors.h and SwitchGovernor.h's
// switch_gov_index_at_fraction, for switch_thermal_sensors_test.cpp.
#ifndef SWITCH_SWITCH_THERMAL_SENSORS_HARNESS_H
#define SWITCH_SWITCH_THERMAL_SENSORS_HARNESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned sensors_count(void);
const char *sensor_fourcc(unsigned i);
const char *sensor_name(unsigned i);
int sensor_source(unsigned i);
int sensor_info_only(unsigned i);
uint64_t sensors_read_mask(void);
unsigned sensor_fourcc_value(unsigned i);
unsigned j181_sensors_count(void);
const char *j181_sensor_fourcc(unsigned i);
const char *j181_sensor_name(unsigned i);
int j181_sensor_source(unsigned i);
int j181_sensor_info_only(unsigned i);
uint64_t j181_sensors_read_mask(void);
int tcal_millic_j120(void);
int tcal_millic_j181(void);
int fixed_of(int milli_c);
int cltm_centi(int fixed);
int cltm_valid(int centi);
int plausible(int milli_c);
int soctherm_millic(unsigned readback);
int max17050_millic(unsigned raw);
unsigned max17050_talrt(int milli_c);
unsigned tsensor_calib(unsigned common, unsigned fuse, unsigned sensor);
unsigned tsensor_base(unsigned sensor);
unsigned tsensor_fuse(unsigned sensor);
const char *tsensor_name(unsigned sensor);
unsigned tsensor_config0(void);
unsigned tsensor_config1(void);
unsigned index_at_fraction(const uint32_t *khz, unsigned count, uint32_t fraction16);
unsigned cltm_ceiling(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
const char *cltm_key(unsigned i);

#ifdef __cplusplus
}
#endif

#endif  // SWITCH_SWITCH_THERMAL_SENSORS_HARNESS_H
