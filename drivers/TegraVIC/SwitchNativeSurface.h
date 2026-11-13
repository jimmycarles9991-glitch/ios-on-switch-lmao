// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchVICPicture.h"
#include "TegraVICSurface.h"
#include "../TegraGM20B/SwitchGpuBacking.h"

struct SwitchVICEvents;

struct SwitchNativeSurfaceCrop {
    uint32_t x, y, width, height;
};

// A destination TegraVIC recorded as a decoder's picture turned instead of copying it (SwitchVICTurn.h): the
// IOSurfaceAccelerator transform (a quarter turn) and the source rectangle in the visible picture, pixels.
struct SwitchNativeSurfaceTurn {
    uint32_t transform;
    uint32_t x, y, width, height;
};

struct SwitchNativeSurfaceView {
    OSObject *owner;
    IOMemoryDescriptor *descriptor;
    switch_gpu_backing *logical;
    switch_gpu_backing *native;
    switch_gpu_backing_guard guard;
    switch_gpu_backing_guard representation;
    switch_gpu_backing_token producer;
    switch_gpu_backing_layout layout;
    SwitchNativeSurfaceCrop crop;
    SwitchNativeSurfaceTurn turn;  // transform 0: the picture as it is
    // A recorded turn's request waits, resolved to their producers: every reader of the picture orders after them.
    const switch_gpu_backing_token *waits;
    uint32_t waitCount;
};

// A view retains both backings. Every engine read submits both uses and both guards in one checked ownership transaction.
// avoid: -16 instead of waiting for a publication in progress, which may itself wait for leases the caller holds.
// turns: a turned picture too (the display); any other reader gets -95 and reads the rows, made on first access.
int switch_native_surface_acquire(const TegraVICSurface *surface, SwitchNativeSurfaceView *view, bool avoid,
                                  bool turns = false);
void switch_native_surface_release(SwitchNativeSurfaceView *view);
// why: on an error, the refusal (a literal) for the caller's one line. fresh: on success, whether the publication
// needed a new surface object rather than a retired one.
int switch_native_surface_publish(const TegraVICSurface *surface, const TegraVICPicture *picture,
                                  const char **why = nullptr, bool *fresh = nullptr);
// `destination` (a whole NV12 surface) is `source`'s picture turned by `op` (a quarter turn of a source rectangle onto
// the whole destination): its rows are made by VIC only when something other than the display reads them. `events`
// (the request's, prepared): taken by the record when it is published, its waits ordered before every reader and its
// signals delivered once (switch_native_surface_deliver, a reader's VIC job, or the record replaced or freed).
int switch_native_surface_publish_turned(const TegraVICSurface *destination, const SwitchNativeSurfaceView &source,
                                         const TegraVICOp &op, uint32_t chromaSite, const char **why = nullptr,
                                         SwitchVICEvents *events = nullptr);
// The display latched (or dropped) the frame `view` shows: a recorded turn's signals go now, bound to its picture's
// completion, unless something delivered them already.
void switch_native_surface_deliver(const SwitchNativeSurfaceView &view);

// The caller holds a descriptor CPU reservation through the ensuing IOSurface CPU lock and unlock.
extern "C" int switch_native_surface_prepare(OSObject *surface, uint32_t id, void *task, uint32_t options,
                                             switch_gpu_backing **logical, switch_gpu_cpu_lease **owned);

// The descriptor reservation and returned logical lease cover checked consumer publication; conversion stays on hardware.
extern "C" int switch_native_surface_prepare_gpu(OSObject *surface, uint32_t id, void *task, uint32_t options,
                                                 switch_gpu_backing **logical, switch_gpu_cpu_lease **owned);

// The caller holds the descriptor CPU reservation; this query neither converts nor waits for GPU work.
extern "C" int switch_native_surface_gpu_admission(OSObject *surface, uint32_t access, switch_gpu_backing *expected);

#if DEVELOPMENT || DEBUG
// Since boot: native publications, and how many of them needed a new surface object (the rest reused a retired one).
extern "C" void switch_native_surface_publications(uint64_t *published, uint64_t *allocated);
#endif
