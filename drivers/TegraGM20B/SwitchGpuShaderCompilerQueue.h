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
#include <IOKit/IOLocks.h>
#include <IOKit/IOUserClient.h>
#include <kern/thread.h>
#include <switch/user_turnstile.h>
#include "TegraGM20BCompilerABI.h"

namespace switch_gpu_compiler
{
constexpr uint32_t kClients = 128;

struct Request {
	Request *next;
	uint64_t id, cookie, size, client;
	const void *owner;
	IOBufferMemoryDescriptor *bytes;  // the request, until it is taken
	uint32_t qos, flags, attempts, how, waiters;
	int32_t pid, result;
	bool async, taken, copying, done, abandoned, sendPending, waitedAsync;
	thread_t inheritor;
	switch_user_turnstile turnstile;
	TegraGM20BAbiCompileTimes times;  // the compiler's, at its Complete
	OSAsyncReference64 reference;
	TegraGM20BAbiCompilerScope
		scope;  // the asking process's, read in its own context at Compile
};

struct Client {
	const void *owner;
	uint64_t id;
};

struct ClientLibrary {
	ClientLibrary *next;
	const void *owner;
	uint64_t client, id, bytes;
	IOMemoryDescriptor *memory;
};

struct CompilerLibraryMap {
	CompilerLibraryMap *next;
	uint64_t client, id, bytes;
	IOMemoryMap *map;
};

struct Broker {
	IOLock *lock;
	const void *compiler, *announced;
	thread_t taker;
	thread_t workers[TegraGM20BCompilerMaxWorkers];
	IOMemoryDescriptor *store;
	uint64_t storeBytes, stack, nextId, nextClient, nextLibraryId;
	uint32_t generation, pending, losses;
	// SwitchGpuShaderCompilerPolicy.h: the losses since the last registration
	uint32_t lossesInARow;
	int32_t compilerPid;  // debug kernels' kern.switch_shader_compiler (SWITCH_GPU_COMPILER_LINE)
	uint64_t queuedBytes, libraryBytesTotal;
	bool declined, restarts;
	Request *requests;  // queued and taken, oldest first
	ClientLibrary *libraries;
	CompilerLibraryMap *libraryMaps;
	Client clients[kClients];
};

extern Broker gBroker;

IOLock *brokerLock();
uint64_t clientId(const void *owner);
void unlink(Request *request);
void dropBytes(Request *request);
void setInheritor(Request *request, thread_t target);
void replaceThread(thread_t &slot, thread_t target);
void freeRequest(Request *request);
void finish(Request *request, int32_t result, Request **sends, uint32_t how = 0);
void send(Request *sends, uint32_t generation);
uint32_t finishAll(int32_t result, Request **sends);
bool reserve(const void *owner, uint64_t size, bool async);
void unreserve(uint64_t size);
bool entitled(task_t task);
bool copyTask(task_t task, uint64_t address, uint64_t bytes, IOBufferMemoryDescriptor *buffer,
	      bool intoBuffer);
IOReturn registerLibrary(const void *owner, task_t task,
			 const TegraGM20BAbiCompilerRegisterLibrary *in,
			 TegraGM20BAbiCompilerRegisterLibraryReply *out);
IOReturn mapLibrary(const void *owner, task_t task, const TegraGM20BAbiCompilerMapLibrary *in,
		    TegraGM20BAbiCompilerMapLibraryReply *out);
IOReturn unmapLibrary(const void *owner, const TegraGM20BAbiCompilerUnmapLibrary *in);
ClientLibrary *takeLibraries(const void *owner);
CompilerLibraryMap *takeLibraryMaps();
void dropLibraries(ClientLibrary *list);
void dropLibraryMaps(CompilerLibraryMap *list);

}  // namespace switch_gpu_compiler
