// SPDX-License-Identifier: MIT
// The kernel side of the system shader compiler's rendezvous (TegraGM20BCompilerABI.h): one broker for the whole system,
// which every TegraGM20B connection reaches through these calls. A connection is named by its user client (`owner`), never
// dereferenced here.
#pragma once
#include <IOKit/IOUserClient.h>
#include "SwitchGpuAsyncReference.h"

bool switch_gpu_compiler_method(uint32_t method);
// One of the methods above, `in` the record (copied, aligned), `out` the reply of exactly `outSize` bytes. Called without
// the connection's own lock: Compile and Take sleep here.
IOReturn switch_gpu_compiler_call(const void *owner, task_t task, uint32_t method, const void *in,
				  uint32_t inSize, void *out, uint32_t outSize,
				  const IOExternalMethodArguments *arguments,
				  SwitchGpuAsyncReference &reference);
// clientMemoryForType for TegraGM20BCompilerStoreToken: the registered store, read only
IOReturn switch_gpu_compiler_store(IOOptionBits *options, IOMemoryDescriptor **memory);
// The connection closed: its queued requests go; the compiler's own close completes every request Unavailable
void switch_gpu_compiler_close(const void *owner);

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#define SWITCH_GPU_COMPILER_LINE 1
// kern.switch_shader_compiler (debug kernels), the HW Test's system.compiler_alive: "pid=N generation=N losses=N
// declined=0|1", the registered compiler's pid (0: none), how many compilers registered this boot, how many were lost
void switch_gpu_compiler_line(char *line, size_t size);
void switch_gpu_compiler_register_sysctl(void);
#else
#define SWITCH_GPU_COMPILER_LINE 0
#endif
