// SPDX-License-Identifier: MIT
#pragma once
#include <objc/runtime.h>
#include <cstdint>

// Instances of `cls` built on the storage its instances' deallocs gave back. Its +allocWithZone: takes a kept block
// (zeroed, as objc_constructInstance requires) or a new one, and its -dealloc runs `dropped`, destroys the instance in
// place (objc_destructInstance: C++ and ARC ivars, associated objects, weak references) and keeps the block for the
// next. A block returns to the pool only from dealloc, so an instance anyone still holds is never handed out again.
// `cls` must not implement -dealloc or +allocWithZone: itself; its subclasses allocate and free as NSObject's do. Call
// before its first instance. False where `cls` was taken already or its methods could not be added.
bool rawPoolInstances(Class cls, void (*dropped)(void *object));

// The blocks the pool of `cls` made rather than took back, and those it freed because it already kept its most, since the
// process began (0 for a class with no pool). With none freed, made is the most instances of `cls` alive at once.
uint64_t rawPoolMade(Class cls);
uint64_t rawPoolFreed(Class cls);
