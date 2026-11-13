// SPDX-License-Identifier: MIT
// A SubmitJob record (TegraGM20BJobABI.h method 33) as the kernel copied it: its parts, or null when malformed.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "TegraGM20BJobABI.h"

struct SwitchGpuJobRecord {
	TegraGM20BAbiJob *job;
	TegraGM20BAbiPush *pushes;
	const TegraGM20BAbiSurfaceUse *uses;
	const TegraGM20BAbiSubmitEvents *events;
	const TegraGM20BAbiSignalRelease *releases;
	uint64_t admission;                        // the admission cookie, 0: none
	const TegraGM20BAbiJobPromises *promises;  // the writer promises it keeps, or null

	static constexpr uint32_t kFlags = TegraGM20BJobExternal | TegraGM20BJobHostWritten |
					   TegraGM20BJobHasEvents | TegraGM20BJobQuiet |
					   TegraGM20BJobAdmission | TegraGM20BJobFulfills |
					   TegraGM20BJobEventReleases;
	static constexpr size_t kMaxBytes =
		sizeof(TegraGM20BAbiJob) + TegraGM20BMaxJobPushes * sizeof(TegraGM20BAbiPush) +
		TegraGM20BMaxJobUses * sizeof(TegraGM20BAbiSurfaceUse) +
		sizeof(TegraGM20BAbiSubmitAdmission) + sizeof(TegraGM20BAbiJobPromises) +
		sizeof(TegraGM20BAbiSubmitEvents) +
		TegraGM20BMaxJobEvents *
			(sizeof(TegraGM20BAbiEventUse) + sizeof(TegraGM20BAbiSignalRelease));

	static SwitchGpuJobRecord parse(void *bytes, size_t size)
	{
		SwitchGpuJobRecord record = {};
		if (!bytes || size < sizeof(TegraGM20BAbiJob) || size > kMaxBytes)
			return record;
		auto *job = static_cast<TegraGM20BAbiJob *>(bytes);
		if (!job->channel || (job->flags & ~kFlags) ||
		    job->set > TegraGM20BMaxResourceSets || (job->set && !job->set_generation) ||
		    (!job->set && job->set_generation) ||
		    job->push_count > TegraGM20BMaxJobPushes ||
		    job->shared_count > TegraGM20BMaxJobUses)
			return record;
		const uint32_t admitted = job->flags & TegraGM20BJobAdmission;
		const bool begins = admitted & TegraGM20BJobAdmissionBegin,
			   continues = admitted & TegraGM20BJobAdmissionContinue;
		if ((begins && continues) ||
		    ((admitted & TegraGM20BJobAdmissionEnd) && !begins && !continues) ||
		    ((admitted & TegraGM20BJobAdmissionCancel) &&
		     (admitted != TegraGM20BJobAdmissionCancel || job->push_count ||
		      job->shared_count)))
			return record;
		size_t expected = sizeof(*job) + job->push_count * sizeof(TegraGM20BAbiPush) +
				  job->shared_count * sizeof(TegraGM20BAbiSurfaceUse);
		auto *pushes = reinterpret_cast<TegraGM20BAbiPush *>(job + 1);
		const auto *uses =
			reinterpret_cast<const TegraGM20BAbiSurfaceUse *>(pushes + job->push_count);
		const auto *after = reinterpret_cast<const uint8_t *>(uses + job->shared_count);
		uint64_t cookie = 0;
		if (admitted) {
			if (size < expected + sizeof(TegraGM20BAbiSubmitAdmission))
				return record;
			const auto *lease =
				reinterpret_cast<const TegraGM20BAbiSubmitAdmission *>(after);
			if (!lease->cookie || lease->reserved)
				return record;
			cookie = lease->cookie;
			expected += sizeof(*lease);
			after += sizeof(*lease);
		}
		const TegraGM20BAbiJobPromises *promises = nullptr;
		if (job->flags & TegraGM20BJobFulfills) {
			if (size < expected + sizeof(TegraGM20BAbiJobPromises))
				return record;
			promises = reinterpret_cast<const TegraGM20BAbiJobPromises *>(after);
			if (!promises->count || promises->count > TegraGM20BMaxJobPromises ||
			    promises->reserved)
				return record;
			expected += sizeof(*promises);
			after += sizeof(*promises);
		}
		const TegraGM20BAbiSubmitEvents *events = nullptr;
		const TegraGM20BAbiSignalRelease *releases = nullptr;
		if ((job->flags & TegraGM20BJobEventReleases) &&
		    !(job->flags & TegraGM20BJobHasEvents))
			return record;
		if (job->flags & TegraGM20BJobHasEvents) {
			if (size < expected + sizeof(TegraGM20BAbiSubmitEvents))
				return record;
			events = reinterpret_cast<const TegraGM20BAbiSubmitEvents *>(after);
			if (events->reserved || events->wait_count > TegraGM20BMaxJobEvents ||
			    events->signal_count > TegraGM20BMaxJobEvents - events->wait_count)
				return record;
			expected += sizeof(*events) + (events->wait_count + events->signal_count) *
							      sizeof(TegraGM20BAbiEventUse);
			const auto *records =
				reinterpret_cast<const TegraGM20BAbiEventUse *>(events + 1);
			if (job->flags & TegraGM20BJobEventReleases) {
				releases = reinterpret_cast<const TegraGM20BAbiSignalRelease *>(
					records + events->wait_count + events->signal_count);
				expected +=
					events->signal_count * sizeof(TegraGM20BAbiSignalRelease);
			}
			for (uint32_t i = 0;
			     size == expected && i < events->wait_count + events->signal_count; ++i)
				if (!records[i].handle)
					return record;
		}
		if (size != expected)
			return record;
		return {job, pushes, uses, events, releases, cookie, promises};
	}

	bool valid() const
	{
		return job != nullptr;
	}
};
