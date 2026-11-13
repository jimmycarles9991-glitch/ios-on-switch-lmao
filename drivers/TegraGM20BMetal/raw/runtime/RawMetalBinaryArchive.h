// SPDX-License-Identifier: MIT
// The device's binary archives (RawMetalBinaryArchive.mm) and Metal's archive miss at a state's creation
#pragma once
#import "RawMetalObjects.h"
#include "raw_archive_keys.h"

id<MTLBinaryArchive> rmNewBinaryArchive(id device, MTLBinaryArchiveDescriptor *descriptor, NSError **error);

// A creation's answer under `options` for the state of archive key `key` (the pack holding it: `packed`), over the archives
// its descriptor lists; asked after the pack lookup and before any compile
raw::ArchiveAnswer rmArchiveAnswer(NSArray *archives, MTLPipelineOption options, uint64_t key);

// Metal's error for a Missed creation: code 3 in the target driver's domain (raw_device_answers.h Kind::driver)
NSError *rmArchiveMissError(NSString *function, const char *pipeline);

uint64_t rmArchiveKey(const std::string &key);

// Whether the pack holds kernels of the compute state (raw_compute_key.h computeStateKey), at any dispatch shape
bool rmPackHoldsKernels(const std::string &stateKey);
// The pack's answers for the compute state of `stateKey` (raw_compute_key.h computeStateKey): null where it has none
const raw::PackComputeFacts *rmPackComputeFacts(const std::string &stateKey);

// A tile creation's answer: false, with Metal's error, where it Missed. m2v's state is made without the archives or the
// option either way: it reads none of this device's archives
bool rmTileArchiveAnswer(MTLTileRenderPipelineDescriptor *twin, MTLTileRenderPipelineDescriptor *descriptor,
                         MTLPipelineOption &options, NSError **error);
