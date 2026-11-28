// SPDX-License-Identifier: BSD-2-Clause-Patent
// switch_host1x_vic_failure_test's two halves: VIC's real queue (the test) on the real TegraHost1x
// (switch_host1x_vic_failure_host1x.cpp), each its own translation unit so their anonymous namespaces stay apart.
#pragma once
#include <stdint.h>

// host1x brought up on a register file whose status bits clear where a one is written and whose CPU increments take.
bool switch_test_host1x_start();
uint32_t &switch_test_sync(uint32_t offset);
