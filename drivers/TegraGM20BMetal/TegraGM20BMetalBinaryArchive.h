// SPDX-License-Identifier: MIT
// The device's binary archives (TegraGM20BMetalBinaryArchive.mm) and Metal's archive miss at a state's creation
#pragma once
#import "TegraGM20BMetalObjects.h"
#include "archive_keys.h"

id<MTLBinaryArchive> gm20bNewBinaryArchive(id device, MTLBinaryArchiveDescriptor *descriptor, NSError **error);

// A creation's answer under `options` for the state of archive key `key` (the pack holding it: `packed`), over the archives
// its descriptor lists; asked after the pack lookup and before any compile
gm20b::ArchiveAnswer gm20bArchiveAnswer(NSArray *archives, MTLPipelineOption options, uint64_t key);

// Metal's error for a Missed creation: code 3 in the target driver's domain (device_answers.h Kind::driver)
NSError *gm20bArchiveMissError(NSString *function, const char *pipeline);

uint64_t gm20bArchiveKey(const std::string &key);

// Whether the pack holds kernels of the compute state (compute_key.h computeStateKey), at any dispatch shape
bool gm20bPackHoldsKernels(const std::string &stateKey);
// The pack's answers for the compute state of `stateKey` (compute_key.h computeStateKey): null where it has none
const gm20b::PackComputeFacts *gm20bPackComputeFacts(const std::string &stateKey);
