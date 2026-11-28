// SPDX-License-Identifier: MIT
#pragma once
#include <objc/runtime.h>
#include <cstdint>

// Instances of `cls` built on the storage its instances' deallocs gave back. Its +allocWithZone: takes a kept block
// (zeroed, as objc_constructInstance requires) or a new one, and its -dealloc runs `dropped`, destroys the instance in
// place (objc_destructInstance: C++ and ARC ivars, associated objects, weak references) and keeps the block for the
// next, up to as many blocks as instances of `cls` were ever alive at once (at most 256): a frame of as many encoders
// as the busiest one before allocates and frees none. A block returns to the pool only from dealloc, so an instance
// anyone still holds is never handed out again.
// `cls` must not implement -dealloc or +allocWithZone: itself; its subclasses allocate and free as NSObject's do. Call
// before its first instance. False where `cls` was taken already or its methods could not be added.
bool gm20bPoolInstances(Class cls, void (*dropped)(void *object), bool threadCache = false);
// An instance of `cls` as +new makes one of a class whose -init is NSObject's (+1), with no message sent
void *gm20bPoolNew(Class cls);

// The blocks the pool of `cls` made rather than took back, and those it freed because it already kept its most, since the
// process began (0 for a class with no pool); the most instances of `cls` alive at once, and the blocks kept now.
uint64_t gm20bPoolMade(Class cls);
uint64_t gm20bPoolFreed(Class cls);
uint32_t gm20bPoolMost(Class cls);
uint32_t gm20bPoolKept(Class cls);
