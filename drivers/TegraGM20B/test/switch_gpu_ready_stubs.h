// SPDX-License-Identifier: MIT
#pragma once
#include "switch_gpu_event_stubs.h"
#define IONew(type, count) static_cast<type *>(malloc(sizeof(type) * (count)))
