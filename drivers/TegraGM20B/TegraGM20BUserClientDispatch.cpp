// SPDX-License-Identifier: MIT
// The ABI dispatch: run's method switch.
#include "TegraGM20BUserClientInternal.h"
#include "TegraGM20BClockABI.h"
#include "TegraGM20BGrPreemptABI.h"
#include "TegraGM20BZcullABI.h"

static IOReturn zbcAdd(const TegraGM20BAbiZbc *in, TegraGM20BAbiZbcReply *out)
{
	if (in->type != TegraGM20BZbcColor && in->type != TegraGM20BZbcDepth)
		return kIOReturnBadArgument;
	const int index = tgm_zbc_add(static_cast<int>(in->type == TegraGM20BZbcDepth), in->format,
				      in->ds, in->l2);
	if (index < 0)
		return ioreturn(index);
	out->index = static_cast<uint32_t>(index);
	out->reserved = 0;
	return kIOReturnSuccess;
}

// IOGPUFamily's for a queue's MTLGPUPriorityRealTime (the same string in 21H461's and 24B119's)
static constexpr char kRealTimeEntitlement[] = "com.apple.private.agx.rt-priority-spi";

// lock_ held. Method 18: the asked priority schedules the connection's channels as IOGPUFamily's setter would; the
// compositor is the one at the top that also holds the UI-critical marker (applyLevel).
IOReturn AGXDeviceUserClient::setPriority(const TegraGM20BAbiPriority &asked)
{
	const int granted = tgm_priority_granted(asked.priority);
	if (granted < 0)
		return kIOReturnBadArgument;
	if (!entitlementsRead_) {
		entitlementsRead_ = true;
		realTimeEntitled_ = switch_gpu_task_entitled(task_, kRealTimeEntitlement);
		restartNoKill_ = switch_gpu_task_entitled(task_, kSwitchGpuUiCriticalEntitlement);
	}
	if (asked.priority == 5 && !realTimeEntitled_ && !realTimeLogged_) {
		realTimeLogged_ = true;
		IOLog("tegra-gm20b: %s[%d] does not have the entitlement to use "
		      "MTLGPUPriorityRealTime, using MTLGPUPriorityHigh instead\n",
		      owner_, ownerPid_);
	}
	if (granted == 0 && !restartNoKill_ && !highLogged_) {
		highLogged_ = true;
		IOLog("tegra-gm20b: %s[%d] asked for GPU priority 0: scheduled high, not the "
		      "compositor\n",
		      owner_, ownerPid_);
	}
	priority_ = uint32_t(granted);
	backgroundPriority_ = asked.background_priority;
	applyLevel();
	return kIOReturnSuccess;
}

static const char *jobCall(uint32_t method)
{
	if (method == TegraGM20BMethodSubmitJob)
		return "SubmitJob";
	return method == TegraGM20BMethodResourceSetCreate ? "ResourceSetCreate" : nullptr;
}

template <typename T> static bool reservedZero(const T &v)
{
	return v == 0;
}

IOReturn AGXDeviceUserClient::run(uint32_t method, const void *in, uint32_t inSize, void *out,
				  const uint32_t *outSize)
{
	// exact record sizes; VmBind also accepts just `count` entries
	uint32_t wantIn = 0;
	uint32_t wantOut = 0;
	uint32_t count = 0;
	switch (method) {
	case TegraGM20BMethodEventImport:
		wantIn = sizeof(TegraGM20BAbiEventImport);
		wantOut = sizeof(TegraGM20BAbiEventImportReply);
		break;
	case TegraGM20BMethodEventRelease:
		wantIn = sizeof(TegraGM20BAbiEventRelease);
		break;
	case TegraGM20BMethodEventWord:
		wantIn = sizeof(TegraGM20BAbiEventWord);
		wantOut = sizeof(TegraGM20BAbiEventWordReply);
		break;
	case TegraGM20BMethodGetInfo:
		wantOut = sizeof(TegraGM20BAbiInfo);
		break;
	case TegraGM20BMethodBoAlloc:
		wantIn = sizeof(TegraGM20BAbiBoAlloc);
		wantOut = sizeof(TegraGM20BAbiBoAllocReply);
		break;
	case TegraGM20BMethodBoImport:
		wantIn = sizeof(TegraGM20BAbiBoImport);
		wantOut = sizeof(TegraGM20BAbiBoAllocReply);
		break;
	case TegraGM20BMethodBoFree:
		wantIn = sizeof(TegraGM20BAbiBoFree);
		break;
	case TegraGM20BMethodBoPark:
		wantIn = sizeof(TegraGM20BAbiBoPark);
		break;
	case TegraGM20BMethodBoUnpark:
		wantIn = sizeof(TegraGM20BAbiBoUnpark);
		wantOut = sizeof(TegraGM20BAbiBoUnparkReply);
		break;
	case TegraGM20BMethodCpuComplete:
		wantIn = sizeof(TegraGM20BAbiCpuComplete);
		wantOut = sizeof(TegraGM20BAbiCpuCompleteReply);
		break;
	case TegraGM20BMethodCpuAcquire:
	case TegraGM20BMethodCpuRelease:
		wantIn = sizeof(TegraGM20BAbiCpuAccess);
		break;
	case TegraGM20BMethodSubmitJob:
		wantIn = sizeof(TegraGM20BAbiSubmitAddress);
		wantOut = sizeof(TegraGM20BAbiExecReply);
		break;
	case TegraGM20BMethodResourceSetCreate:
		wantIn = sizeof(TegraGM20BAbiResourceSetCreate);
		wantOut = sizeof(TegraGM20BAbiResourceSetCreateReply);
		break;
	case TegraGM20BMethodResourceSetRelease:
		wantIn = sizeof(TegraGM20BAbiResourceSetRelease);
		break;
	case TegraGM20BMethodWriterPromise:
		wantIn = sizeof(TegraGM20BAbiWriterPromise);
		wantOut = sizeof(TegraGM20BAbiWriterPromiseReply);
		break;
	case TegraGM20BMethodVmBind:
		if (inSize < offsetof(TegraGM20BAbiVmBind, ops))
			return kIOReturnBadArgument;
		count = static_cast<const TegraGM20BAbiVmBind *>(in)->count;
		if (!count || count > TegraGM20BMaxBindOps)
			return kIOReturnBadArgument;
		wantIn = inSize == sizeof(TegraGM20BAbiVmBind)
				 ? inSize
				 : static_cast<uint32_t>(offsetof(TegraGM20BAbiVmBind, ops) +
							 count * sizeof(TegraGM20BAbiBindOp));
		break;
	case TegraGM20BMethodChannelCreate:
		wantIn = sizeof(TegraGM20BAbiChannelCreate);
		wantOut = sizeof(TegraGM20BAbiChannelCreateReply);
		break;
	case TegraGM20BMethodChannelDestroy:
		wantIn = sizeof(TegraGM20BAbiChannel);
		break;
	case TegraGM20BMethodWaitFence:
		wantIn = sizeof(TegraGM20BAbiWaitFence);
		break;
	case TegraGM20BMethodGetStatus:
		wantIn = sizeof(TegraGM20BAbiChannel);
		wantOut = sizeof(TegraGM20BAbiStatus);
		break;
	case TegraGM20BMethodMap:
		wantIn = sizeof(TegraGM20BAbiMap);
		wantOut = sizeof(TegraGM20BAbiMapReply);
		break;
	case TegraGM20BMethodUnmap:
		wantIn = sizeof(TegraGM20BAbiUnmap);
		break;
	case TegraGM20BMethodSurfaceWrites:
		wantIn = sizeof(TegraGM20BAbiSurfaceWrites);
		break;
	case TegraGM20BMethodNativeSurfaceView:
		wantIn = sizeof(TegraGM20BAbiNativeSurfaceView);
		wantOut = sizeof(TegraGM20BAbiNativeSurfaceViewReply);
		break;
	case TegraGM20BMethodSurfaceLayoutInfo:
		wantIn = sizeof(TegraGM20BAbiSurfaceLayout);
		wantOut = sizeof(TegraGM20BAbiSurfaceLayoutInfo);
		break;
	case TegraGM20BMethodSurfaceLayout:
		wantIn = sizeof(TegraGM20BAbiSurfaceLayout);
		wantOut = sizeof(TegraGM20BAbiSurfaceLayoutReply);
		break;
	case TegraGM20BMethodPriority:
		wantIn = sizeof(TegraGM20BAbiPriority);
		break;
	case TegraGM20BMethodZbc:
		wantIn = sizeof(TegraGM20BAbiZbc);
		wantOut = sizeof(TegraGM20BAbiZbcReply);
		break;
	case TegraGM20BMethodZcullInfo:
		wantOut = sizeof(TegraGM20BAbiZcullInfo);
		break;
	case TegraGM20BMethodClockAnchors:
		wantOut = sizeof(TegraGM20BAbiClockAnchorsReply);
		break;
	case TegraGM20BMethodPackImageMap:
		wantIn = sizeof(TegraGM20BAbiPackImageMap);
		wantOut = sizeof(TegraGM20BAbiPackImageMapReply);
		break;
#if TGM_STATS
	case TegraGM20BMethodReadback:
		wantIn = sizeof(TegraGM20BAbiReadback);
		wantOut = sizeof(TegraGM20BAbiReadbackReply);
		break;
	case TegraGM20BMethodRegdump:
		wantIn = sizeof(TegraGM20BAbiRegdump);
		wantOut = sizeof(TegraGM20BAbiRegdumpReply);
		break;
	case TegraGM20BMethodCopyReset:
		wantIn = sizeof(TegraGM20BAbiCopyReset);
		wantOut = sizeof(TegraGM20BAbiCopyResetReply);
		break;
	case TegraGM20BMethodGrPreempt:
		wantIn = sizeof(TegraGM20BAbiGrPreempt);
		wantOut = sizeof(TegraGM20BAbiGrPreemptReply);
		break;
#endif
#if SWITCH_GPU_PATH_STATS
	case TegraGM20BMethodPaths:
		wantIn = inSize == sizeof(TegraGM20BAbiPathsSpan) ? sizeof(TegraGM20BAbiPathsSpan)
								  : sizeof(TegraGM20BAbiPaths);
		wantOut = wantIn == sizeof(TegraGM20BAbiPathsSpan)
				  ? 0
				  : sizeof(TegraGM20BAbiPathsReply);
		break;
#endif
	default:
		return kIOReturnUnsupported;
	}
	if (inSize != wantIn || *outSize != wantOut || (wantIn && !in) || (wantOut && !out)) {
		if (const char *call = jobCall(method))
			refuseCall(call, SwitchGpuRefusal::RecordSize, kIOReturnBadArgument, inSize,
				   *outSize);
		return kIOReturnBadArgument;
	}
	if (tgm_stage() < 2) {
		if (const char *call = jobCall(method))
			refuseCall(call, SwitchGpuRefusal::GpuDown, kIOReturnNotReady,
				   uint64_t(tgm_stage()));
		return kIOReturnNotReady;
	}

	switch (method) {
	case TegraGM20BMethodBoImport:
		if (static_cast<const TegraGM20BAbiBoImport *>(in)->flags &
		    ~static_cast<uint32_t>(TegraGM20BImportGpuReads))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodBoPark:
		if (!reservedZero(static_cast<const TegraGM20BAbiBoPark *>(in)->reserved) ||
		    (static_cast<const TegraGM20BAbiBoPark *>(in)->flags &
		     ~static_cast<uint32_t>(TegraGM20BParkGuard)))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodBoUnpark:
		if (!reservedZero(static_cast<const TegraGM20BAbiBoUnpark *>(in)->reserved) ||
		    static_cast<const TegraGM20BAbiBoUnpark *>(in)->flags)
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodSurfaceWrites:
		if (!reservedZero(static_cast<const TegraGM20BAbiSurfaceWrites *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodSurfaceLayoutInfo:
	case TegraGM20BMethodSurfaceLayout:
		if (!reservedZero(static_cast<const TegraGM20BAbiSurfaceLayout *>(in)->reserved) ||
		    (static_cast<const TegraGM20BAbiSurfaceLayout *>(in)->flags &
		     ~static_cast<uint32_t>(TegraGM20BLayoutWrites)))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodVmBind:
		if (!reservedZero(static_cast<const TegraGM20BAbiVmBind *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodChannelCreate:
		if (!reservedZero(static_cast<const TegraGM20BAbiChannelCreate *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodChannelDestroy:
	case TegraGM20BMethodGetStatus:
		if (!reservedZero(static_cast<const TegraGM20BAbiChannel *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodUnmap:
		if (!reservedZero(static_cast<const TegraGM20BAbiUnmap *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodPriority:
		if (static_cast<const TegraGM20BAbiPriority *>(in)->priority >
			    TegraGM20BMaxPriority ||
		    static_cast<const TegraGM20BAbiPriority *>(in)->background_priority >
			    TegraGM20BMaxPriority)
			return kIOReturnBadArgument;
		break;
	default:
		break;
	}

	if (method == TegraGM20BMethodEventImport) {
		const auto *input = static_cast<const TegraGM20BAbiEventImport *>(in);
		auto *reply = static_cast<TegraGM20BAbiEventImportReply *>(out);
		if (input->reserved || !input->port)
			return kIOReturnBadArgument;
		return ioreturn(
			events_.import(task_, input->port, &reply->handle, &reply->currentValue));
	}
	if (method == TegraGM20BMethodEventRelease)
		return ioreturn(events_.release(
			static_cast<const TegraGM20BAbiEventRelease *>(in)->handle));
	if (method == TegraGM20BMethodEventWord) {
		const auto *input = static_cast<const TegraGM20BAbiEventWord *>(in);
		auto *reply = static_cast<TegraGM20BAbiEventWordReply *>(out);
		*reply = {};
		if (input->reserved || !input->channel || input->channel > kMaxChannels)
			return kIOReturnBadArgument;
		return ioreturn(events_.word(input->handle, input->channel, &reply->address,
					     &reply->base, &reply->floor));
	}
	if (method == TegraGM20BMethodSubmitJob)
		return submitJob(static_cast<const TegraGM20BAbiSubmitAddress *>(in),
				 static_cast<TegraGM20BAbiExecReply *>(out));
	if (method == TegraGM20BMethodResourceSetCreate)
		return resourceSetCreate(static_cast<const TegraGM20BAbiResourceSetCreate *>(in),
					 static_cast<TegraGM20BAbiResourceSetCreateReply *>(out));
	if (method == TegraGM20BMethodWriterPromise) {
		if (static_cast<const TegraGM20BAbiWriterPromise *>(in)->op == TegraGM20BPromiseArm)
			return armPromises(static_cast<const TegraGM20BAbiWriterPromise *>(in));
		IOLockLock(lock_);
		const IOReturn promised =
			submitGate_.closed()
				? kIOReturnNotOpen
				: writerPromise(
					  static_cast<const TegraGM20BAbiWriterPromise *>(in),
					  static_cast<TegraGM20BAbiWriterPromiseReply *>(out));
		IOLockUnlock(lock_);
		return promised;
	}
	if (method == TegraGM20BMethodWaitFence)
		return waitFence(static_cast<const TegraGM20BAbiWaitFence *>(in));
	if (method == TegraGM20BMethodCpuComplete)
		return cpuComplete(static_cast<const TegraGM20BAbiCpuComplete *>(in),
				   static_cast<TegraGM20BAbiCpuCompleteReply *>(out));
	if (method == TegraGM20BMethodCpuAcquire || method == TegraGM20BMethodCpuRelease) {
		const auto *access = static_cast<const TegraGM20BAbiCpuAccess *>(in);
		const IOReturn ret = cpuAccess(access, method == TegraGM20BMethodCpuRelease);
		if (method == TegraGM20BMethodCpuAcquire && ret == kIOReturnBadArgument &&
		    cpuAcquireParkedRefusal(access))
			return kIOReturnNotReady;
		return ret;
	}
	// BoAlloc and BoImport make the BO without lock_ (the wiring, zero-fill and clean are the calling thread's own) and
	// take it to publish the BO. Map and Unmap take lock_ themselves (Map's clientMemoryForType must run without it).
	if (method == TegraGM20BMethodBoAlloc)
		return boAlloc(static_cast<const TegraGM20BAbiBoAlloc *>(in),
			       static_cast<TegraGM20BAbiBoAllocReply *>(out));
	if (method == TegraGM20BMethodBoImport)
		return boImport(static_cast<const TegraGM20BAbiBoImport *>(in),
				static_cast<TegraGM20BAbiBoAllocReply *>(out));
	if (method == TegraGM20BMethodBoPark)
		return boPark(static_cast<const TegraGM20BAbiBoPark *>(in));
	if (method == TegraGM20BMethodBoUnpark)
		return boUnpark(static_cast<const TegraGM20BAbiBoUnpark *>(in),
				static_cast<TegraGM20BAbiBoUnparkReply *>(out));
	if (method == TegraGM20BMethodBoFree)
		return boFreeMethod(static_cast<const TegraGM20BAbiBoFree *>(in));
	if (method == TegraGM20BMethodMap) {
		return mapToken(static_cast<const TegraGM20BAbiMap *>(in),
				static_cast<TegraGM20BAbiMapReply *>(out));
	}
	if (method == TegraGM20BMethodUnmap)
		return unmapToken(static_cast<const TegraGM20BAbiUnmap *>(in));
	if (method == TegraGM20BMethodPackImageMap)
		return packImageMap(static_cast<const TegraGM20BAbiPackImageMap *>(in),
				    static_cast<TegraGM20BAbiPackImageMapReply *>(out));
	if (method == TegraGM20BMethodClockAnchors) {
		*static_cast<TegraGM20BAbiClockAnchorsReply *>(out) = {
			fence_gpu_ + TegraGM20BClockAnchorOffset, kFenceToken, 0};
		return kIOReturnSuccess;
	}
	if (method == TegraGM20BMethodZbc) {
		return zbcAdd(static_cast<const TegraGM20BAbiZbc *>(in),
			      static_cast<TegraGM20BAbiZbcReply *>(out));
	}
	if (method == TegraGM20BMethodZcullInfo)
		return tgm_zcull_info(static_cast<TegraGM20BAbiZcullInfo *>(out))
			       ? kIOReturnUnsupported
			       : kIOReturnSuccess;
#if TGM_STATS
	if (method == TegraGM20BMethodReadback) {
		return tgm_readback_call(this, static_cast<const TegraGM20BAbiReadback *>(in),
					 static_cast<TegraGM20BAbiReadbackReply *>(out));
	}
	if (method == TegraGM20BMethodRegdump) {
		return tgm_regdump_log(static_cast<const TegraGM20BAbiRegdump *>(in),
				       static_cast<TegraGM20BAbiRegdumpReply *>(out))
			       ? kIOReturnBadArgument
			       : kIOReturnSuccess;
	}
	if (method == TegraGM20BMethodCopyReset) {
		const int ret =
			tgm_copy_reset_call(static_cast<const TegraGM20BAbiCopyReset *>(in),
					    static_cast<TegraGM20BAbiCopyResetReply *>(out));
		if (ret == -ENOMEM)
			return kIOReturnNoMemory;
		return ret ? kIOReturnBadArgument : kIOReturnSuccess;
	}
#endif
#if SWITCH_GPU_PATH_STATS
	if (method == TegraGM20BMethodPaths) {
		const uint32_t op = static_cast<const TegraGM20BAbiPaths *>(in)->op;
		auto *reply = static_cast<TegraGM20BAbiPathsReply *>(out);
		if ((op == TegraGM20BPathsOpNoteSpan) != (inSize == sizeof(TegraGM20BAbiPathsSpan)))
			return kIOReturnBadArgument;
		if (op == TegraGM20BPathsOpNoteSpan) {
			TegraGM20BAbiPathsSpan note =
				*static_cast<const TegraGM20BAbiPathsSpan *>(in);
			note.text[sizeof(note.text) - 1] = 0;
			switch_gpu_span_note(owner_, note.text);
		} else if (op == TegraGM20BPathsOpSpans) {
			switch_gpu_span_line(reply->line, sizeof(reply->line));
		} else if (op == TegraGM20BPathsOpCounts)
			switch_gpu_path_line(reply->line, sizeof(reply->line));
		else if (op == TegraGM20BPathsOpPool)
			poolLine(reply->line, sizeof(reply->line));
		else if (op == TegraGM20BPathsOpRefusals)
			snprintf(reply->line, sizeof(reply->line), "lines=%u",
				 __atomic_load_n(&refusalLines_, __ATOMIC_RELAXED));
#if TGM_STATS
		else if (op == TegraGM20BPathsOpTargetPeaks)
			tgm_target_peaks_line(reply->line, sizeof(reply->line));
		else if (op == TegraGM20BPathsOpForeignWaits) {
			struct tgm_foreign_waits totals = {};
			tgm_foreign_waits_totals(&totals);
			snprintf(reply->line, sizeof(reply->line),
				 "foreign_jobs=%llu foreign_fences=%llu foreign_promises=%llu "
				 "foreign_events=%llu foreign_host1x=%llu",
				 static_cast<unsigned long long>(totals.jobs),
				 static_cast<unsigned long long>(totals.fences),
				 static_cast<unsigned long long>(totals.promises),
				 static_cast<unsigned long long>(totals.events),
				 static_cast<unsigned long long>(totals.host1x));
		}
#endif
		else
			return kIOReturnBadArgument;
		return kIOReturnSuccess;
	}
#endif
	IOLockLock(lock_);
	// A layout decision holds what it uses itself and drops lock_ while the copy engine may move the surface.
	const bool mutation = switch_gpu_method_closes_gate(method);
	if (submitGate_.closed() || (mutation && !submitGate_.beginMutation(lock_))) {
		IOLockUnlock(lock_);
		return kIOReturnNotOpen;
	}
	// A channel killed for a GPU fault or hang (client fault isolation) refuses only what names it (SubmitJob and
	// WaitFence answer kIOReturnNotResponding, GetStatus its error); the connection's memory, binds and other channels
	// go on, and ChannelCreate makes it a new one, as a GPU restart leaves a Metal device on iOS.
	IOReturn ret = 0;
	switch (method) {
	case TegraGM20BMethodGetInfo:
		ret = getInfo(static_cast<TegraGM20BAbiInfo *>(out));
		break;
	case TegraGM20BMethodNativeSurfaceView:
		ret = nativeSurfaceView(static_cast<const TegraGM20BAbiNativeSurfaceView *>(in),
					static_cast<TegraGM20BAbiNativeSurfaceViewReply *>(out));
		break;
	case TegraGM20BMethodVmBind:
		ret = vmBindParkedRefusal(static_cast<const TegraGM20BAbiVmBind *>(in), count);
		if (!ret)
			ret = vmBind(static_cast<const TegraGM20BAbiVmBind *>(in), count);
		break;
	case TegraGM20BMethodChannelCreate:
		ret = channelCreate(static_cast<const TegraGM20BAbiChannelCreate *>(in),
				    static_cast<TegraGM20BAbiChannelCreateReply *>(out));
		break;
	case TegraGM20BMethodChannelDestroy:
		ret = channelDestroy(static_cast<const TegraGM20BAbiChannel *>(in));
		break;
	case TegraGM20BMethodGetStatus:
		ret = getStatus(static_cast<const TegraGM20BAbiChannel *>(in),
				static_cast<TegraGM20BAbiStatus *>(out));
		break;
	case TegraGM20BMethodSurfaceWrites:
		tgm_coherency_gpu_writes(
			static_cast<const TegraGM20BAbiSurfaceWrites *>(in)->surface_id);
		ret = kIOReturnSuccess;
		break;
	case TegraGM20BMethodSurfaceLayoutInfo:
		ret = surfaceLayoutInfo(static_cast<const TegraGM20BAbiSurfaceLayout *>(in),
					static_cast<TegraGM20BAbiSurfaceLayoutInfo *>(out), true);
		break;
	case TegraGM20BMethodSurfaceLayout:
		ret = surfaceLayout(static_cast<const TegraGM20BAbiSurfaceLayout *>(in),
				    static_cast<TegraGM20BAbiSurfaceLayoutReply *>(out));
		break;
	case TegraGM20BMethodResourceSetRelease:
		ret = resourceSetRelease(static_cast<const TegraGM20BAbiResourceSetRelease *>(in));
		break;
	case TegraGM20BMethodPriority:
		ret = setPriority(*static_cast<const TegraGM20BAbiPriority *>(in));
		break;
#if TGM_STATS
	case TegraGM20BMethodGrPreempt: {
		const auto *input = static_cast<const TegraGM20BAbiGrPreempt *>(in);
		auto *reply = static_cast<TegraGM20BAbiGrPreemptReply *>(out);
		*reply = {};
		if (input->reserved[0] || input->reserved[1] || !input->channel ||
		    input->channel > kMaxChannels || !chans_[input->channel - 1] ||
		    input->mode > TegraGM20BGrPreemptCta) {
			ret = kIOReturnBadArgument;
			break;
		}
		ret = ioreturn(tgm_chan_gr_preempt(chans_[input->channel - 1], input->mode,
						   &reply->old_mode, &reply->readback));
		break;
	}
#endif
	default:
		ret = kIOReturnUnsupported;
		break;
	}
	if (mutation)
		submitGate_.endMutation(lock_);
	IOLockUnlock(lock_);
	return ret;
}
