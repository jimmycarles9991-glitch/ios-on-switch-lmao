// SPDX-License-Identifier: MIT
// kits/SwitchLinux and shim/xnu_glue.cpp as gpu_exec.c and nvif's channel reach them, on the host. A panic or an nvkm
// warning fails the test.
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/test/exec/switch_gpu_exec_fixture.h"

extern "C" {
void *switch_exec_host_alloc(uint64_t bytes)
{
	return std::calloc(1, bytes);
}

void switch_exec_host_free(void *block)
{
	std::free(block);
}

void *switch_linux_mutex_alloc(void)
{
	return new std::mutex;
}

void switch_linux_mutex_free(void *mutex)
{
	delete static_cast<std::mutex *>(mutex);
}

void *switch_linux_mutex_of(void **slot)
{
	if (!*slot)
		*slot = switch_linux_mutex_alloc();
	return *slot;
}

void switch_linux_mutex_lock(void *mutex) __attribute__((no_thread_safety_analysis))
{
	static_cast<std::mutex *>(mutex)->lock();
}

void switch_linux_mutex_unlock(void *mutex) __attribute__((no_thread_safety_analysis))
{
	static_cast<std::mutex *>(mutex)->unlock();
}

bool switch_linux_mutex_trylock(void *mutex) __attribute__((no_thread_safety_analysis))
{
	return static_cast<std::mutex *>(mutex)->try_lock();
}

uint64_t switch_linux_nanotime(void)
{
	return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
				.count());
}

void switch_linux_wakeup(const void *)
{
}

void switch_linux_udelay(uint64_t)
{
}

void nvxnu_log(const char *format, ...)
{
	char line[512];
	va_list arguments;
	va_start(arguments, format);
	std::vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	ADD_FAILURE() << "nvxnu_log: " << line;
}

void nvxnu_stats(const char *, ...)
{
}

[[noreturn]] void nvxnu_panic(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	std::vfprintf(stderr, format, arguments);
	va_end(arguments);
	std::fputc('\n', stderr);
	std::abort();
}

void nvxnu_kdebug(uint32_t, uint64_t, uint64_t, uint64_t)
{
}

uint32_t nvxnu_wake_seq(void)
{
	return 0;
}

int nvxnu_idle(uint32_t, uint64_t)
{
	return 0;
}
}
