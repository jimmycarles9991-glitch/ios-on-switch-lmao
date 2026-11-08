// Host harness for drivers/SwitchPower/SwitchSensors.h (thermalmonitord's sensor maps, the selected one and J181AP's, unit conversions, SOC_THERM fuse
// calibration, CLTM's CPU ceiling) and SwitchGovernor.h's switch_gov_index_at_fraction as exported C functions (C11
// with -Wconversion -Wsign-conversion), driven by switch_thermal_sensors_test.cpp.
#include <stdint.h>
#include "../SwitchGovernor.h"
#include "../SwitchSensors.h"
#include "switch_thermal_sensors_harness.h"

unsigned sensors_count(void)
{
    return SWITCH_SENSOR_COUNT;
}

const char *sensor_fourcc(unsigned i)
{
    return i < SWITCH_SENSOR_COUNT ? switch_sensors[i].fourcc : 0;
}

const char *sensor_name(unsigned i)
{
    return i < SWITCH_SENSOR_COUNT ? switch_sensors[i].matchName : 0;
}

int sensor_source(unsigned i)
{
    return i < SWITCH_SENSOR_COUNT ? (int)switch_sensors[i].source : -1;
}

int sensor_info_only(unsigned i)
{
    return i < SWITCH_SENSOR_COUNT ? switch_sensors[i].info_only : -1;
}

uint64_t sensors_read_mask(void)
{
    return switch_sensors_read_mask();
}

unsigned sensor_fourcc_value(unsigned i)
{
    return switch_sensor_fourcc(switch_sensors[i].fourcc);
}

unsigned j181_sensors_count(void)
{
    return SWITCH_SENSOR_COUNT_J181AP;
}

const char *j181_sensor_fourcc(unsigned i)
{
    return i < SWITCH_SENSOR_COUNT_J181AP ? switch_sensors_j181ap[i].fourcc : 0;
}

const char *j181_sensor_name(unsigned i)
{
    return i < SWITCH_SENSOR_COUNT_J181AP ? switch_sensors_j181ap[i].matchName : 0;
}

int j181_sensor_source(unsigned i)
{
    return i < SWITCH_SENSOR_COUNT_J181AP ? (int)switch_sensors_j181ap[i].source : -1;
}

int j181_sensor_info_only(unsigned i)
{
    return i < SWITCH_SENSOR_COUNT_J181AP ? switch_sensors_j181ap[i].info_only : -1;
}

uint64_t j181_sensors_read_mask(void)
{
    return switch_sensor_table_read_mask(switch_sensors_j181ap, SWITCH_SENSOR_COUNT_J181AP);
}

int fixed_of(int milli_c)
{
    return switch_sensor_fixed(milli_c);
}

int cltm_centi(int fixed)
{
    return switch_sensor_cltm_centi(fixed);
}

int cltm_valid(int centi)
{
    return switch_sensor_cltm_valid(centi);
}

int plausible(int milli_c)
{
    return switch_sensor_plausible(milli_c);
}

int soctherm_millic(unsigned readback)
{
    return switch_soctherm_millic(readback);
}

int max17050_millic(unsigned raw)
{
    return switch_max17050_millic((uint16_t)raw);
}

unsigned max17050_talrt(int milli_c)
{
    return switch_max17050_talrt(milli_c);
}

unsigned tsensor_calib(unsigned common, unsigned fuse, unsigned sensor)
{
    const struct switch_tsensor_shared shared = switch_tsensor_shared_calib(common);
    return switch_tsensor_calib(&switch_tsensor_b01, &shared, &switch_tsensors[sensor], fuse);
}

unsigned tsensor_base(unsigned sensor)
{
    return switch_tsensors[sensor].base;
}

unsigned tsensor_fuse(unsigned sensor)
{
    return switch_tsensors[sensor].calib_fuse;
}

const char *tsensor_name(unsigned sensor)
{
    return switch_tsensors[sensor].name;
}

unsigned tsensor_config0(void)
{
    return switch_tsensor_config0(&switch_tsensor_b01);
}

unsigned tsensor_config1(void)
{
    return switch_tsensor_config1(&switch_tsensor_b01);
}

unsigned index_at_fraction(const uint32_t *khz, unsigned count, uint32_t fraction16)
{
    return switch_gov_index_at_fraction(khz, count, fraction16);
}

unsigned cltm_ceiling(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    const uint32_t f[SWITCH_CLTM_CONTRIBUTORS] = {a, b, c, d};
    return switch_cltm_ceiling(f);
}

const char *cltm_key(unsigned i)
{
    return i < SWITCH_CLTM_CONTRIBUTORS ? switch_cltm_keys[i] : 0;
}
