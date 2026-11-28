// Force-included into drivers/TegraI2C/TegraI2C.cpp's host build (test_switch_touch.py, TEGRA_I2C_HOST_MODEL): its
// register accessors and DMA cache maintenance, which tests/switch_touch_harness.cpp implements over its controller
// model.
#pragma once
#include <cstddef>
#include <cstdint>
uint32_t r32(volatile uint8_t *base, uint32_t offset);
void w32(volatile uint8_t *base, uint32_t offset, uint32_t value);
void dmaInvalidate(const uint8_t *va, size_t size);
