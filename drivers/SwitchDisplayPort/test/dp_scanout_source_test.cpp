// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::SourceText;

TEST(ExternalOwnership, FrontReadLivesUntilReplacementAndGenerationChangesRefuseReuse)
{
    const auto source = SourceText("../SwitchExternalScanout.cpp");
    const auto present = SourceText("../SwitchExternalPresent.cpp");
    const auto latch = Block(present, "bool SwitchExternalScreen::frameEnd(");
    EXPECT_TRUE(latch.has("if (latched && dcb->latch())"));
    EXPECT_TRUE(latch.before("promoteDrawing();", "retire(pending, true);"));
    EXPECT_TRUE(latch.has("switch_native_scanout_deliver(&slots[k].read);"));
    EXPECT_FALSE(latch.has("switch_native_scanout_release"));
    const auto promote = Block(present, "void SwitchExternalScreen::promoteDrawing()");
    EXPECT_TRUE(promote.has("if (k >= 0 && !slotShown(k)) releaseRead(static_cast<unsigned>(k));"));

    const auto reuse = Block(source, "if (slot.read.lease)");
    EXPECT_TRUE(reuse.has("slot.read.allocation.content_generation != snapshot.allocation.content_generation"));
    EXPECT_TRUE(reuse.has("switch_native_scanout_release(&snapshot); return -1;"));
    EXPECT_FALSE(reuse.has("releaseRead"));
    EXPECT_FALSE(reuse.has("slot.read ="));

    const auto lifecycle = SourceText("../SwitchExternalScreen.cpp");
    const auto off = Block(lifecycle, "void SwitchExternalScreen::linkDown()");
    EXPECT_TRUE(off.has("releaseRead(static_cast<unsigned>(k));"));
    EXPECT_TRUE(off.has("abandonDrawing();"));
    const auto detach = Block(lifecycle, "void SwitchExternalScreen::detach()");
    EXPECT_TRUE(detach.has("releaseSlot(i);"));
    EXPECT_TRUE(detach.has("drainSwapQueue();"));
    EXPECT_TRUE(detach.has("publishActiveGeometry();"));
    EXPECT_TRUE(Block(lifecycle, "void SwitchExternalScreen::modeFailed()").has("drainSwapQueue();"));
    const auto select = Block(lifecycle, "IOReturn SwitchExternalScreen::selectOutput(");
    EXPECT_TRUE(select.has("self->publishActiveGeometry();"));
    EXPECT_TRUE(select.has("if (device != 2) self->drainSwapQueue();"));
    EXPECT_TRUE(Block(lifecycle, "void SwitchExternalScreen::linkUp(").has("mode.hactive != timing.hactive"));

    // Layer records outlive a link drop (a retrain mid-video keeps the inherited UI) and follow drained swaps too.
    EXPECT_FALSE(off.has("clearLayerState"));
    EXPECT_TRUE(Block(lifecycle, "void SwitchExternalScreen::linkUp(").has("releaseSlot(i); clearLayerState(); }"));
    EXPECT_TRUE(detach.has("clearLayerState();"));
    EXPECT_TRUE(Block(lifecycle, "void SwitchExternalScreen::drainSwapQueue()")
                    .before("recordLayers(reinterpret_cast<const uint8_t *>(request));", "retire(request, false)"));
    // Unplugged, a drained swap's surfaces are not recorded: detach cleared the records and nothing would release them.
    EXPECT_TRUE(Block(lifecycle, "void SwitchExternalScreen::drainSwapQueue()")
                    .has("if (connected) recordLayers(reinterpret_cast<const uint8_t *>(request));"));
    EXPECT_TRUE(Block(lifecycle, "void SwitchExternalScreen::linkUp(").has("unshownErrors = 0;"));
    EXPECT_TRUE(Block(source, "void SwitchExternalScreen::recordLayers(").has("surface->retain();"));
    EXPECT_TRUE(Block(source, "void SwitchExternalScreen::requestFrame()")
                    .before("(native_planes::wordAt(bytes, swapUpdated) & ~native_planes::wordAt(bytes, swapReady)) & "
                            "native_planes::kLayerBits",
                            "takeLayers(bytes, present, layers, surfaces)"));
    EXPECT_TRUE(Block(source, "void SwitchExternalScreen::requestFrame()")
                    .has("if (!takeLayers(bytes, present, layers, surfaces)) { dropFrame(request); return; }"));

    const auto validate = Block(lifecycle, "bool SwitchExternalScreen::validateSwap(");
    EXPECT_TRUE(validate.has("__atomic_load_n(&activeGeometry, __ATOMIC_ACQUIRE)"));
    EXPECT_FALSE(validate.has("modes.modes[selectedMode]"));
    EXPECT_TRUE(validate.has("switch_external::checkSwap(updated, background, hasSurface, layers, width, height)"));
    EXPECT_FALSE(validate.has("swapReady"));

    const auto prepare = Block(source, "int SwitchExternalScreen::prepareSlot(");
    EXPECT_TRUE(prepare.has("bytes > memory->getLength()"));
    EXPECT_TRUE(prepare.has("if (!slot.hold && !applyLayout("));
    EXPECT_TRUE(prepare.has("!slotShown(static_cast<int>(i)) && !(building & (1u << i))"));

    const auto request = Block(source, "void SwitchExternalScreen::requestFrame()");
    EXPECT_TRUE(request.has("switch_external::imageFor(layers[i], slotBlock(slots[prepared])"));
    EXPECT_TRUE(request.has("switch_external::planSwap(present, layers, images, states, width, height, plan)"));
    EXPECT_TRUE(request.has("switch_external::frameOf(plan, images, iova, dcb->pixelClockHz())"));
    EXPECT_TRUE(
        request.has("drawing[w] = plan.window[w].on ? slotOf[plan.window[w].layer] : -1; releaseUnshown(building);"));
    // An accepted swap is shown whole or not at all: a layer it cannot place now retires it unshown, and one no window
    // could ever read panics.
    EXPECT_TRUE(request.before("if (prepared < 0)", "unshownFrame(prepared, i, building, request); return;"));
    EXPECT_TRUE(request.has("unscannable(i);"));
    EXPECT_TRUE(request.has("if (plan.unshown) unscannable(static_cast<unsigned>(__builtin_ctz(plan.unshown)));"));
    EXPECT_FALSE(request.has("present[i] = false"));
    EXPECT_TRUE(
        request.has("convert ? prepareConversion(surfaces[i], i, building) : prepareSlot(surfaces[i], building)"));
    const auto convert = SourceText("../SwitchExternalConvert.cpp");
    EXPECT_TRUE(Block(convert, "void SwitchExternalScreen::unshownFrame(").has("dropFrame(request);"));
    EXPECT_TRUE(Block(convert, "void SwitchExternalScreen::unscannable(").has("panic("));
    EXPECT_TRUE(Block(convert, "int SwitchExternalScreen::runConversions()").has("switch_vic_scanout_convert("));
    EXPECT_TRUE(Block(convert, "int SwitchExternalScreen::prepareConversion(").has("betterVictim(i, victim, c.bytes)"));
    EXPECT_TRUE(Block(convert, "bool SwitchExternalScreen::betterVictim(").has("return slot.conversion;"));

    const auto complete = Block(present, "void SwitchExternalScreen::completionFired(");
    EXPECT_TRUE(complete.has("const bool showing = self->connected && self->selectedMode >= 0 && self->device == 2;"));
    EXPECT_TRUE(complete.has("if (!showing) self->drainSwapQueue();"));
    // A conversion waiting for the GPU is tried again when it is ready, as prepareSlot's -16 and STALE are.
    EXPECT_TRUE(complete.before("retryable(status)) { self->pending = nullptr; self->retryWhenReady(); return; }",
                                "self->retire(self->pending, false);"));

    const auto limits = Block(lifecycle, "void SwitchExternalScreen::publishScanoutLimits()");
    EXPECT_FALSE(limits.has("IOMFBYUV420UnSupported"));
    EXPECT_TRUE(limits.has("IOMFBDisableFlip"));
    EXPECT_TRUE(limits.has("switch_external::kPublished"));
    EXPECT_TRUE(
        limits.has("if (switch_external::kPackedYuvUnsupported) setProperty(\"IOMFBYUVPackedUnSupported\", true);"));
    EXPECT_TRUE(limits.has("publishSourceLimits(SWITCH_DP_MAX_PIXEL_KHZ * 1000u)"));
    EXPECT_NE(lifecycle.find("SwitchExternalScreen::getMatrix("), std::string::npos);
    EXPECT_NE(lifecycle.find("SwitchExternalScreen::setMatrix("), std::string::npos);
    EXPECT_NE(lifecycle.find("SwitchExternalScreen::swapSetColorMatrix("), std::string::npos);
    EXPECT_NE(lifecycle.find("SwitchExternalScreen::setAccessibilityScale("), std::string::npos);
    EXPECT_FALSE(Block(lifecycle, "IOReturn SwitchExternalScreen::setAccessibilityScale(").has("colorLock"));
    EXPECT_TRUE(validate.before("if ((updated & (1u << i)) == 0u || !surfaces[i])", "if (!geometry) return false;"));
    const auto kick = Block(present, "void SwitchExternalScreen::kickSwapQueue()");
    EXPECT_TRUE(kick.before("if (!connected || device != 2 || selectedMode < 0)", "drainSwapQueue();"));

    const auto header = SourceText("../SwitchExternalScreen.h");
    EXPECT_NE(header.find("kSlotSpan = 68ULL << 20"), std::string::npos);
    EXPECT_NE(header.find("static_assert(kTablePages == 137);"), std::string::npos);
    EXPECT_EQ(header.find("getWorkLoop()"), std::string::npos);
    EXPECT_NE(header.find("setPowerState(unsigned long ordinal, IOService *whatDevice) override"), std::string::npos);
    EXPECT_NE(header.find("void stop(IOService *provider) override"), std::string::npos);

    const auto service = SourceText("../SwitchDisplayPort.cpp");
    const auto beginPort = Block(service, "bool SwitchDisplayPort::begin()");
    EXPECT_TRUE(beginPort.before("screen = OSTypeAlloc(SwitchExternalScreen);", "loop = screen->getWorkLoop();"));
    EXPECT_TRUE(beginPort.before("loop = screen->getWorkLoop();", "loop->retain();"));
    EXPECT_TRUE(beginPort.before("loop->retain();", "screen->begin(this, loop)"));
    EXPECT_FALSE(beginPort.has("IOWorkLoop::workLoop()"));

    const auto beginScreen = Block(lifecycle, "bool SwitchExternalScreen::begin(");
    EXPECT_TRUE(beginScreen.before("setInternalDisplay(false);", "IOMobileFramebufferLegacy::start(nub)"));
    EXPECT_TRUE(beginScreen.before("setProperty(\"IOMFBDisplayExternal\", kOSBooleanTrue);",
                                   "IOMobileFramebufferLegacy::start(nub)"));
    EXPECT_FALSE(beginScreen.has("registerService();"));
    EXPECT_TRUE(beginScreen.before("PMinit();", "nub->joinPMtree(this);"));
    EXPECT_TRUE(beginScreen.before("nub->joinPMtree(this);", "registerPowerDriver(this, states, 2)"));
    EXPECT_TRUE(beginScreen.before("registerPowerDriver(this, states, 2)", "changePowerStateToPriv(1)"));
    EXPECT_TRUE(beginScreen.has("fenceCall = thread_call_allocate_with_options("));
    EXPECT_FALSE(beginScreen.has("fenceLoop"));
    EXPECT_TRUE(
        beginPort.has("IOFilterInterruptEventSource::filterInterruptEventSource(this, &dcInterrupt, &dcFilter"));

    const auto power = Block(lifecycle, "IOReturn SwitchExternalScreen::setPowerState(");
    EXPECT_TRUE(power.has("if (ordinal > 1) return kIOReturnUnsupported;"));
    EXPECT_TRUE(power.has("IOMobileFramebufferLegacy::setPowerState(ordinal, whatDevice)"));
    EXPECT_TRUE(power.has("self->drainSwapQueue();"));
    EXPECT_TRUE(power.has("self->kickSwapQueue();"));

    const auto publish = Block(lifecycle, "bool SwitchExternalScreen::publish(");
    EXPECT_TRUE(publish.before("setProperty(\"IOMFBHotplugKeysChangedKey\", kOSBooleanTrue);",
                               "IOMobileFramebufferLegacy::hotPlugNotify(1);"));
    const auto clearProps = Block(lifecycle, "void SwitchExternalScreen::clearSinkProperties()");
    EXPECT_TRUE(clearProps.has("removeProperty(\"IOMFBHotplugKeysChangedKey\");"));
    const auto publishGeom = Block(lifecycle, "void SwitchExternalScreen::publishActiveGeometry()");
    EXPECT_TRUE(publishGeom.has("connected && device == 2 && selectedMode >= 0"));

    EXPECT_TRUE(detach.has("setDisplayWidth(0);"));
    EXPECT_TRUE(detach.has("setDisplayHeight(0);"));
    EXPECT_TRUE(Block(lifecycle, "void SwitchExternalScreen::linkUp(").has("dcb->frameInterrupt(true);"));
    EXPECT_TRUE(select.before("self->device = device;", "self->transport->selectMode(index, device == 2)"));
    EXPECT_TRUE(Block(source, "native_planes::Layer SwitchExternalScreen::layerOf(")
                    .has("l.transform = native_planes::wordAt(request, swapTransforms + 4u * layer) & 7u;"));

    const auto mapDom = Block(source, "bool SwitchExternalScreen::mapDomain()");
    EXPECT_TRUE(mapDom.has("(1u << 2) | (1u << 4) | (1u << 6)"));
    // The tables' 548 KiB contiguous run is taken at boot; a dock hours in found none and every swap searched again.
    EXPECT_TRUE(beginScreen.before("switch_dma_alloc(&tables, kTablePages * 4096u)",
                                   "domain.reserve(kSlotBase, kSlots * kSlotSpan)"));
    EXPECT_TRUE(
        beginScreen.before("domain.reserve(kSlotBase, kSlots * kSlotSpan)", "IOMobileFramebufferLegacy::start(nub)"));
    EXPECT_FALSE(mapDom.has("switch_dma_alloc"));
    EXPECT_FALSE(mapDom.has("domain.init"));
    EXPECT_FALSE(mapDom.has("1u << 17"));
    EXPECT_TRUE(prepare.has("allocation.pages[offset / 4096]"));
    EXPECT_FALSE(prepare.has("allocation.pages[0]"));

    EXPECT_FALSE(latch.has("return false;"));
    EXPECT_FALSE(latch.has("abandonDrawing"));
    EXPECT_FALSE(Block(present, "void SwitchExternalScreen::latchFrame()").has("dropFrame"));
    EXPECT_TRUE(latch.has("dcb->frameInterrupt(needFrameEnd);"));
    EXPECT_TRUE(Block(source, "void SwitchExternalScreen::requestFrame()").has("dcb->frameInterrupt(true);"));

    const auto dcIrq = Block(service, "void SwitchDisplayPort::dcInterrupt(");
    EXPECT_FALSE(dcIrq.has("self->fail(\"DCB\""));

    const auto selectMode = Block(service, "IOReturn SwitchDisplayPort::selectMode(");
    EXPECT_TRUE(selectMode.has("gate->runAction("));
    EXPECT_TRUE(selectMode.has("}, &req);"));
    EXPECT_FALSE(selectMode.has("this, &req"));
    EXPECT_TRUE(selectMode.has("commandSleep(&self->trainingEvent, deadline, THREAD_UNINT)"));
    EXPECT_TRUE(selectMode.has("self->stage == Stage::Running && self->wantedMode == r.index"));

    const auto session = SourceText("../SwitchDPSession.cpp");
    EXPECT_TRUE(Block(session, "void SwitchDisplayPort::trained()").has("wakeModeWaiters(false);"));

    const auto teardown = Block(service, "void SwitchDisplayPort::teardownLink()");
    EXPECT_TRUE(teardown.before("dcb.powerDown();", "sor.powerDown();"));
    EXPECT_TRUE(teardown.before("dcb.windowsOff();", "dcb.powerDown();"));
    const auto auxTimer = Block(service, "void SwitchDisplayPort::auxTimerFired(");
    EXPECT_FALSE(auxTimer.has("panic("));
    EXPECT_TRUE(auxTimer.has("self->aux.resetAux();"));
    EXPECT_TRUE(auxTimer.has("self->cecComplete(false);"));
    EXPECT_TRUE(auxTimer.has("self->fail(\"DPAUX1\", why);"));

    EXPECT_TRUE(publish.before("tegra_gm20b_set_external_scanout(1);", "IOMobileFramebufferLegacy::hotPlugNotify(1);"));
    EXPECT_TRUE(detach.has("tegra_gm20b_set_external_scanout(0);"));
    EXPECT_TRUE(validate.before("cachedFirstPa(memory)", "memory->prepare(kIODirectionOut)"));
    EXPECT_TRUE(validate.before("memory->prepare(kIODirectionOut)",
                                "memory->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone)"));
    EXPECT_TRUE(validate.before("memory->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone)",
                                "memory->complete(kIODirectionOut);"));
    EXPECT_TRUE(validate.has("tegra_gm20b_surface_compressed(firstPa & ~0xfffULL)"));
    EXPECT_TRUE(validate.has("cacheValidatedLayers(request, id, updated, hasSurface, surfaces, layers);"));
    EXPECT_TRUE(Block(source, "void SwitchExternalScreen::recordLayers(").has("lookupValidatedLayers("));
    const auto layout = Block(source, "bool SwitchExternalScreen::applyLayout(");
    EXPECT_TRUE(
        layout.before("tegra_gm20b_surface_hold(firstPage, &tiled)", "tegra_gm20b_surface_compressed(firstPage)"));
    EXPECT_FALSE(layout.has("__atomic_thread_fence(__ATOMIC_SEQ_CST);"));
    EXPECT_FALSE(prepare.has("__atomic_thread_fence(__ATOMIC_SEQ_CST);"));
    EXPECT_TRUE(prepare.has("tegra_gm20b_surface_compressed(firstPage)"));
    EXPECT_TRUE(prepare.has("if (heldError == -16) return -16;"));
    EXPECT_TRUE(Block(lifecycle, "bool SwitchExternalScreen::getRefreshAttributes(").has("if (!refreshPeriod)"));
    EXPECT_TRUE(publish.before("native_legacy_set_display_refresh_properties(this)",
                               "IOMobileFramebufferLegacy::hotPlugNotify(1);"));
}

// One fence over every window's producers: the media engines' syncpoints when only VIC and NVDEC write the surfaces,
// otherwise a non-blocking GPU bridge fence directly on the work loop, deferring to fenceCall only for VIC conversions
// or a full bridge ring (-16).
TEST(ExternalOwnership, OneFenceOverEveryWindow)
{
    const auto present = SourceText("../SwitchExternalPresent.cpp");
    const auto gather = Block(present, "uint32_t SwitchExternalScreen::gatherFence(");
    EXPECT_TRUE(gather.has("switch_native_scanout_dependencies(read, own, 3 + SWITCH_GPU_BACKING_PROMISES)"));
    EXPECT_FALSE(gather.has("switch_gpu_backing_promise_tokens("));
    EXPECT_TRUE(gather.has("switch_gpu_backing_token_external_status(&own[t]) == 1"));
    const auto fence = Block(present, "void SwitchExternalScreen::fenceFrame()");
    EXPECT_TRUE(
        fence.before("if (!count) { gpuPending = false; latchFrame(); return; }", "switch_host1x_token_fence("));
    EXPECT_TRUE(fence.has("media && count <= SWITCH_HOST1X_TOKEN_FENCE_MAX"));
    EXPECT_TRUE(fence.has("if (result != -16 && result != -22)"));
    EXPECT_TRUE(
        fence.before("switch_gpu_bridge_fence(fenceTokens, count, fenced, this)", "thread_call_enter(fenceCall)"));
    const auto bridge = Block(present, "void SwitchExternalScreen::fenceWork()");
    // VIC's jobs run on the slow-path thread call, and the frame's producers are gathered after them.
    EXPECT_TRUE(bridge.before("runConversions()", "gatherFence(media)"));
    EXPECT_TRUE(bridge.has("switch_gpu_bridge_fence("));
    EXPECT_TRUE(bridge.before("if (error != -16) break;", "error = switch_gpu_bridge_wait_room();"));
    EXPECT_TRUE(bridge.has("gpuComplete(error);"));
    EXPECT_EQ(present.find("tegra_gm20b_scanout_copy"), std::string::npos);
    EXPECT_EQ(SourceText("../SwitchExternalScanout.cpp").find("tegra_gm20b_scanout_copy"), std::string::npos);
}
