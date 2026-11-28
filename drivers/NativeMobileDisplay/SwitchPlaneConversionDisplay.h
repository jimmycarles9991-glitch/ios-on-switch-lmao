// SPDX-License-Identifier: BSD-2-Clause-Patent
struct ConvertedPlane {
    switch_gpu_backing *backing = nullptr;
    uint64_t bytes = 0, allocation = 0, allocationGeneration = 0, content = 0;
    uint32_t width = 0, height = 0, cropX = 0, cropY = 0, format = 0, matrix = 0;
    uint64_t offset = 0, planeOffset[2] = {};
    uint32_t transfer = 0, primaries = 0, chromaTop = 0, chromaBottom = 0;
    uint32_t stride[2] = {}, rows[2] = {};
    uint32_t transform = 0;
    native_planes::ScreenConversion shown = {};
    bool valid = false;
};

static constexpr unsigned kConvertedPlanes = 4;
ConvertedPlane *convertedPlanes = nullptr;

struct PlaneConversionJob {
    const OriginalLegacySwapRequest *request;
    ConvertedPlane value;
    TegraVICImage input;
    TegraVICOp operation;
    SwitchNativeScanout output;
    int sourceSlot, target, status;
    uint32_t pitch;
    uint64_t bytes;
};

PlaneConversionJob *planeConversion = nullptr;
bool planeConversionQueued = false;

bool planeConversionUses(int slot) const
{
    return planeConversionQueued && planeConversion->sourceSlot == slot;
}

// A submission refused behind a failed source reports that source's plain failure: its producer says why.
void planeConversionFinished()
{
    auto &work = *planeConversion;
    const int status =
        work.status ? producerFailure(work.status, &planeSlots[work.sourceSlot].native->view.producer, 1) : 0;
    auto &target = convertedPlanes[work.target];
    if (target.backing)
        switch_gpu_backing_release(target.backing);
    target = work.value;
    work.value.backing = nullptr;
    target.valid = !work.status;
    switch_native_scanout_release(&work.output);
    planeConversionQueued = false;
    transferred = false;
    if (copyStopping)
        return;
    if (!work.status) {
        kickSwapQueue();
    } else if ((latchRetire ? firstUntaken() : pendingSwapRequest()) == work.request) {
        if (sourceUnshown(status, work.request, "overlay conversion")) {
            noteTaken(work.request, mach_absolute_time());
            planeNextSet = false;
            retireSwapUnshown(work.request, kNoBuffer);
        } else {
            native_legacy_transfer_observed(kIOReturnNotReady, work.request, 0);
        }
    }
}

void planeConversionWork()
{
    auto &work = *planeConversion;
    work.status = kIOReturnAborted;
    if (!__atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE)) {
        if (!work.value.backing || work.value.bytes < work.bytes) {
            if (work.value.backing)
                switch_gpu_backing_release(work.value.backing);
            work.value.backing = switch_display_scanout_buffer(size_t(work.bytes));
            work.value.bytes = work.value.backing ? work.bytes : 0;
        }
        work.status = work.value.backing ? switch_vic_scanout_convert(planeSlots[work.sourceSlot].native, &work.input,
                                                                      work.value.backing, uint32_t(work.operation.dstW),
                                                                      uint32_t(work.operation.dstH), work.pitch,
                                                                      &work.operation, &work.output)
                                         : -12;
    }
    getWorkLoop()->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->planeConversionFinished();
            return kIOReturnSuccess;
        },
        this);
}

void stopPlaneConversion()
{
    if (!planeConversion)
        return;
    switch_native_scanout_release(&planeConversion->output);
    if (planeConversion->value.backing)
        switch_gpu_backing_release(planeConversion->value.backing);
    planeConversionQueued = false;
    IODelete(planeConversion, PlaneConversionJob, 1);
    planeConversion = nullptr;
}

bool convertedBusy(switch_gpu_backing *backing) const
{
    for (unsigned slot = 0; slot < kPlaneSlots; ++slot)
        if (planeSlots[slot].native && planeSlots[slot].native->view.native == backing && planeSlotBusy(int(slot)))
            return true;
    return false;
}

void convertedReleaseIdle(switch_gpu_backing *backing)
{
    for (unsigned slot = 0; slot < kPlaneSlots; ++slot)
        if (planeSlots[slot].native && planeSlots[slot].native->view.native == backing && !planeSlotBusy(int(slot)))
            planeReleaseLease(slot);
}

void convertedStop()
{
    if (!convertedPlanes)
        return;
    for (unsigned i = 0; i < kConvertedPlanes; ++i) {
        auto &buffer = convertedPlanes[i];
        if (buffer.backing)
            switch_gpu_backing_release(buffer.backing);
        buffer = {};
    }
    IODelete(convertedPlanes, ConvertedPlane, kConvertedPlanes);
    convertedPlanes = nullptr;
}

// VIC's input for a packed RGB layer the conversion job turns: its rows where the surface keeps them, or the GPU's blocks
// read in place (TegraVIC's blockLinearRgbIn).
static const char *packedConversionInput(const native_planes::Layer &layer, const SwitchNativeScanout *source,
                                         TegraVICImage *input)
{
    TegraVICSurfaceDesc desc = {};
    desc.fourcc = layer.surface.format;
    desc.width = layer.surface.width;
    desc.height = layer.surface.height;
    desc.planes = 1;
    desc.length = layer.surface.descriptorBytes;
    desc.planeBytesPerRow[0] = layer.surface.stride[0];
    desc.planeHeight[0] = layer.surface.height;
    const char *why = tegra_vic_image_from(&desc, source->pitchOffset, input);
    if (why)
        return why;
    tgm_layout block = {};
    const auto &allocation = source->allocation;
    if (!allocation.page_count || !allocation.pages ||
        tegra_gm20b_surface_layout(allocation.pages[0] & ~0xfffULL, &block) != 1)
        return nullptr;
    // The layout describes the backing's first page: a surface placed further in has its blocks elsewhere, as
    // switch_vic_tiled_layout refuses too.
    if (source->pitchOffset)
        return "a tiled packed conversion source at an offset";
    if (block.bpp != 4 || block.width < layer.surface.width || block.height < layer.surface.height || block.y_log2 > 5)
        return "packed conversion source block layout";
    input->blockLinear = 1;
    input->blockHeightLog2 = block.y_log2;
    input->pitch[0] = block.row_bytes;
    input->rows[0] = block.rows;
    input->span[0] = block.bytes;
    return nullptr;
}

int planeConvertSlot(int sourceSlot, const native_planes::Layer &layer, uint32_t width, uint32_t height,
                     native_planes::Image &image, const OriginalLegacySwapRequest *request = nullptr)
{
    const auto *source = planeSlots[sourceSlot].native;
    const bool packed = layer.kind == native_planes::kKindBgra || layer.kind == native_planes::kKindRgba;
    if (!source || !source->lease || (!native_planes::isYuv(layer.kind) && !packed))
        return -95;
    // The plane is the layer's part on the screen, turned by VIC, at most the source's pixels (NativePlanesTurn.h).
    native_planes::ScreenConversion shown = {};
    if (!native_planes::screenConversionOf(layer, width, height, shown)) {
        image = native_planes::Image();
        return sourceSlot;
    }
    static_assert(native_planes::kConvertedMinimumSide == TEGRA_VIC_MIN_DIM, "VIC's smallest surface side");
    const uint32_t outWidth = uint32_t(shown.width), outHeight = uint32_t(shown.height);
    const uint32_t pitch = (outWidth * 4 + 255) & ~255u;
    const uint64_t bytes = uint64_t(pitch) * outHeight;
    if (!bytes || bytes > kPlaneSlotSpan)
        return -95;
    if (!convertedPlanes)
        convertedPlanes = IONewZero(ConvertedPlane, kConvertedPlanes);
    if (!convertedPlanes)
        return -12;
    const auto &allocation = source->allocation;
    const auto &view = source->view;
    const uint32_t matrix = view.layout.plane_count ? view.layout.color_matrix : layer.surface.colorMatrix;
    // Window B shows a matrix VIC lacks as BT.601 (native_planes::matrixOf); the conversion shows it the same.
    const uint32_t shownMatrix = matrix < SWITCH_VIC_MATRIX_UNSUPPORTED ? matrix : uint32_t(SWITCH_VIC_MATRIX_601);
    const uint32_t transfer = view.layout.plane_count ? view.layout.transfer : layer.surface.transfer;
    const uint32_t primaries = view.layout.plane_count ? view.layout.primaries : layer.surface.primaries;
    const uint32_t chromaTop = view.layout.plane_count ? view.layout.chroma_top : layer.surface.chromaTop;
    const uint32_t chromaBottom = view.layout.plane_count ? view.layout.chroma_bottom : layer.surface.chromaBottom;
    ConvertedPlane *selected = nullptr;
    bool cached = false;
    for (unsigned i = 0; i < kConvertedPlanes; ++i) {
        auto &buffer = convertedPlanes[i];
        bool samePlanes = buffer.offset == source->pitchOffset;
        for (unsigned p = 0; p < 2; ++p)
            samePlanes = samePlanes && buffer.stride[p] == layer.surface.stride[p] &&
                         buffer.planeOffset[p] == layer.surface.offset[p] &&
                         buffer.rows[p] == layer.surface.planeHeight[p];
        if (samePlanes && buffer.valid && buffer.allocation == allocation.allocation_id &&
            buffer.allocationGeneration == allocation.allocation_generation &&
            buffer.content == allocation.content_generation && buffer.width == layer.surface.width &&
            buffer.height == layer.surface.height && buffer.cropX == view.crop.x && buffer.cropY == view.crop.y &&
            buffer.format == layer.surface.format && buffer.matrix == matrix && buffer.transfer == transfer &&
            buffer.primaries == primaries && buffer.chromaTop == chromaTop && buffer.chromaBottom == chromaBottom &&
            buffer.transform == layer.transform && native_planes::screenConversionShows(buffer.shown, shown)) {
            selected = &buffer;
            cached = true;
            break;
        }
        if (!selected && (!buffer.backing || !convertedBusy(buffer.backing)))
            selected = &buffer;
    }
    if (!selected)
        return -11;
    SwitchNativeScanout result = {};
    if (cached) {
        const int error = switch_gpu_backing_export_read(selected->backing, &result.view.producer, &result.lease);
        if (error)
            return error;
        result.view.native = selected->backing;
        switch_gpu_backing_retain(result.view.native);
        switch_gpu_backing_describe(result.view.native, &result.allocation);
    } else {
        TegraVICImage input = {};
        TegraVICOp operation = {};
        operation.srcX = shown.srcX;
        operation.srcY = shown.srcY;
        operation.srcW = shown.srcW;
        operation.srcH = shown.srcH;
        operation.dstW = outWidth;
        operation.dstH = outHeight;
        operation.transform = layer.transform & 7;
        const char *why = nullptr;
        if (packed) {
            why = packedConversionInput(layer, source, &input);
        } else if (view.layout.plane_count) {
            auto picture = view;
            picture.layout.color_matrix = shownMatrix;
            why = switch_native_vic_image(picture, 0, &input);
            if (!why && !switch_native_vic_crop(view, &operation))
                why = "native conversion crop";
        } else {
            TegraVICSurfaceDesc desc = {};
            desc.fourcc = layer.surface.format;
            desc.width = layer.surface.width;
            desc.height = layer.surface.height;
            desc.planes = layer.surface.planes > 1 ? layer.surface.planes : 1;  // NV12 and NV16 2, a packed 4:2:2 1
            desc.length = layer.surface.descriptorBytes;
            desc.colorMatrix = shownMatrix;
            desc.transfer = transfer;
            desc.primaries = primaries;
            desc.chromaTop = chromaTop;
            desc.chromaBottom = chromaBottom;
            for (unsigned p = 0; p < desc.planes && p < 2; ++p) {
                desc.planeOffset[p] = layer.surface.offset[p];
                desc.planeBytesPerRow[p] = layer.surface.stride[p];
                desc.planeHeight[p] = layer.surface.planeHeight[p];
            }
            why = tegra_vic_image_from(&desc, source->pitchOffset, &input);
        }
        if (why)
            return -95;
        if (!copySource || copyStopping)
            return -19;
        if (transferred || planeConversionQueued)
            return -11;
        if (!planeConversion)
            planeConversion = IONewZero(PlaneConversionJob, 1);
        if (!planeConversion)
            return -12;
        if (selected->backing)
            convertedReleaseIdle(selected->backing);
        selected->valid = false;
        auto &work = *planeConversion;
        work = {};
        work.request = request;
        work.sourceSlot = sourceSlot;
        work.target = int(selected - convertedPlanes);
        work.input = input;
        work.operation = operation;
        work.pitch = pitch;
        work.bytes = bytes;
        work.value = *selected;
        if (work.value.backing)
            switch_gpu_backing_retain(work.value.backing);
        auto *value = &work.value;
        value->allocation = allocation.allocation_id;
        value->allocationGeneration = allocation.allocation_generation;
        value->content = allocation.content_generation;
        value->width = layer.surface.width;
        value->height = layer.surface.height;
        value->cropX = view.crop.x;
        value->cropY = view.crop.y;
        value->format = layer.surface.format;
        value->matrix = matrix;
        value->transfer = transfer;
        value->primaries = primaries;
        value->chromaTop = chromaTop;
        value->chromaBottom = chromaBottom;
        value->transform = layer.transform;
        value->shown = shown;
        value->offset = source->pitchOffset;
        for (unsigned p = 0; p < 2; ++p) {
            value->stride[p] = layer.surface.stride[p];
            value->planeOffset[p] = layer.surface.offset[p];
            value->rows[p] = layer.surface.planeHeight[p];
        }
        planeConversionQueued = transferred = true;
        copySource->interruptOccurred(nullptr, nullptr, 0);
        return -115;
    }
    native_planes::ScreenConversion plane = shown;
    plane.width = selected->shown.width;
    plane.height = selected->shown.height;
    const uint32_t planePitch = (uint32_t(plane.width) * 4 + 255) & ~255u;
    const native_planes::Layer rgb = native_planes::screenConversionLayer(plane, planePitch);
    if (!native_planes::imageOf(rgb, width, height, image)) {
        switch_native_scanout_release(&result);
        return -95;
    }
    return planeMapSnapshot(result, image, sourceSlot);
}
