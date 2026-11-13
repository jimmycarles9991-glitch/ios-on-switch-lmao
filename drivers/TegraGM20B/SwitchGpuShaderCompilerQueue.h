// SPDX-License-Identifier: MIT
// The shader compiler broker's requests and their life (SwitchGpuShaderCompiler.cpp's methods use them). Who frees a request:
// a synchronous one, the thread that asked while it waits (it sleeps on the request until it is finished, and returns),
// else finish(); an asynchronous one, finish() once its result is sent, or the asker's close while it is still queued. A
// request whose asker is gone (its connection closed, or its waiting thread was aborted) is finished without delivering.
// One being copied out to the compiler is never freed under Take's feet. A request keeps its bytes until it is finished: one
// the compiler held when it went is queued again for the next.
#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>

namespace switch_gpu_compiler
{
constexpr uint32_t kClients = 128;

struct Request {
	Request *next;
	uint64_t id, cookie, size, client;
	const void *owner;
	IOBufferMemoryDescriptor *bytes;  // the request, until it is taken
	uint32_t qos, flags, attempts, how;
	int32_t pid, result;
	bool async, waiter, taken, copying, done, abandoned;
	OSAsyncReference64 reference;
};

struct Client {
	const void *owner;
	uint64_t id;
};

struct Broker {
	IOLock *lock;
	const void *compiler, *announced;
	IOMemoryDescriptor *store;
	uint64_t storeBytes, stack, nextId, nextClient;
	uint32_t generation, pending, losses;
	// SwitchGpuShaderCompilerPolicy.h: the losses since the last registration
	uint32_t lossesInARow;
	int32_t compilerPid;  // debug kernels' kern.switch_shader_compiler (SWITCH_GPU_COMPILER_LINE)
	uint64_t queuedBytes;
	bool declined, restarts;
	Request *requests;  // queued and taken, oldest first
	Client clients[kClients];
};

extern Broker gBroker;

IOLock *brokerLock();
uint64_t clientId(const void *owner);
void unlink(Request *request);
void dropBytes(Request *request);
void freeRequest(Request *request);
void finish(Request *request, int32_t result, Request **sends, uint32_t how = 0);
void send(Request *sends, uint32_t generation);
uint32_t finishAll(int32_t result, Request **sends);
bool reserve(const void *owner, uint64_t size, bool async);
void unreserve(uint64_t size);
bool entitled(task_t task);
bool copyTask(task_t task, uint64_t address, uint64_t bytes, IOBufferMemoryDescriptor *buffer,
	      bool intoBuffer);

}  // namespace switch_gpu_compiler
