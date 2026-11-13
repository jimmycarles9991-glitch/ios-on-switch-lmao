// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "drivers/TegraGM20B/SwitchGpuBacking.h"

struct SwitchHost1xResult;

bool switch_host1x_test_start(uint32_t engine, uint32_t syncpoint);
SwitchHost1xResult *switch_host1x_test_produce(uint32_t engine, uint64_t first, uint64_t last,
                                               switch_gpu_backing_token *token);
int switch_host1x_test_producer_status(const switch_gpu_backing_token *token);
void switch_host1x_test_complete(uint32_t engine, SwitchHost1xResult **record, int result);

SwitchHost1xResult *switch_host1x_test_consumer_allocate(uint32_t engine, uint64_t sequence,
                                                         switch_gpu_backing_token *token);
int switch_host1x_test_status(const switch_gpu_backing_token *token);
int switch_host1x_test_physical_status(const switch_gpu_backing_token *token);
void switch_host1x_test_physical(uint32_t engine, SwitchHost1xResult *record, int result);
void switch_host1x_test_semantic(const switch_gpu_backing_token *token, int result);
int switch_host1x_test_retain(const switch_gpu_backing_token *token);
void switch_host1x_test_release(const switch_gpu_backing_token *token);
int switch_host1x_test_history(uint32_t engine, uint64_t sequence);
