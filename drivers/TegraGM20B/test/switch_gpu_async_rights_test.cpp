// SPDX-License-Identifier: MIT
// The wake port's send right of an asynchronous call to the TegraGM20B user client, on every return: SwitchGpuAsyncReference
// (externalMethod's settle()), EventObserve (SwitchGpuUserEvents.h) and ResourceObserve (SwitchGpuUserResources.h) over a
// stand-in user client. The MIG server's rule: an error return has it release the right, a success leaves it to the
// method, which releases it once (now, or when its notification is sent).
#include <gtest/gtest.h>

#include <vector>

#include "SwitchGpuAsyncReference.h"
#include "TegraGM20BABI.h"
#include "switch_gpu_async_rights_stubs.h"

struct switch_gpu_backing;
constexpr uint32_t SWITCH_GPU_BACKING_READ = 1;

static IOReturn ioreturn(int err)
{
	return !err         ? kIOReturnSuccess
	       : err == -22 ? kIOReturnBadArgument
	       : err == -28 ? kIOReturnNoResources
			    : kIOReturnError;
}

struct SwitchGpuResourceUse {
	switch_gpu_backing *backing;
	OSObject *surface;
	const volatile uint64_t *generation;
	uint64_t expected;
	uint32_t access;
};

namespace
{
using Callback = void (*)(void *context, uint64_t cookie, int result);

struct Watcher {
	int refuse = 0;
	Callback callback = nullptr;
	void *context = nullptr;
	uint64_t cookie = 0;

	int arm(uint64_t armed, Callback done, void *argument)
	{
		if (refuse)
			return refuse;
		callback = done;
		context = argument;
		cookie = armed;
		return 0;
	}

	void fire(int result)
	{
		const Callback done = callback;
		callback = nullptr;
		done(context, cookie, result);
	}
};

struct Events : Watcher {
	int observe(uint64_t, uint64_t, uint64_t cookie, Callback done, void *argument, bool)
	{
		return arm(cookie, done, argument);
	}

	int cancel(uint64_t, uint64_t)
	{
		return 0;
	}

	int probe(uint64_t, uint64_t, bool)
	{
		return 0;
	}
};

struct Resources : Watcher {
	int observe(const SwitchGpuResourceUse *, uint32_t, uint64_t cookie, Callback done,
		    void *argument)
	{
		return arm(cookie, done, argument);
	}

	int cancel(uint64_t)
	{
		return 0;
	}
};

struct Gate {
	bool shut = false;

	bool closed() const
	{
		return shut;
	}
};
}  // namespace

class AGXDeviceUserClient : public IOUserClient
{
public:
	struct EventNotification {
		AGXDeviceUserClient *owner;
		OSAsyncReference64 reference;
	};

	struct NativeView {
		switch_gpu_backing *logical, *native;
	};

	struct Bo {
		uint64_t handle;
		switch_gpu_backing *backing;
		OSObject *surface;
		const volatile uint64_t *representation;
		uint64_t representationValue;
		bool readOnly;
		NativeView *nativeView;
	};

	struct Binding {
		const Bo *bo;
	};

	Events events_;
	Resources resources_;
	Gate submitGate_;
	IOLock *lock_ = IOLockAlloc();
	std::vector<Bo> bos_;
	const Binding *binds_ = nullptr;
	uint32_t nbinds_ = 0;

	~AGXDeviceUserClient() override
	{
		IOLockFree(lock_);
	}

	const Bo *findBo(uint64_t handle) const
	{
		for (const Bo &bo : bos_)
			if (bo.handle == handle)
				return &bo;
		return nullptr;
	}

	static int copyin(uint64_t address, void *out, size_t bytes)
	{
		memcpy(out, reinterpret_cast<const void *>(address), bytes);
		return 0;
	}

	static void eventNotified(void *context, uint64_t cookie, int result);
	IOReturn eventObserve(const TegraGM20BAbiEventObserve *input,
			      IOExternalMethodArguments *arguments,
			      SwitchGpuAsyncReference &reference);
	IOReturn resourceObserve(const TegraGM20BAbiResourceObserve *, IOExternalMethodArguments *,
				 SwitchGpuAsyncReference &);
};

static const AGXDeviceUserClient::Binding *
switch_gpu_binding_find(const AGXDeviceUserClient::Binding *, uint32_t, uint64_t, uint64_t)
{
	return nullptr;
}

static bool switch_gpu_binding_offset(const AGXDeviceUserClient::Binding &, uint64_t, uint64_t,
				      uint64_t &)
{
	return false;
}

#include "SwitchGpuUserEvents.h"
#include "SwitchGpuUserResources.h"

namespace
{
constexpr mach_port_t kPort = 0x5550;

// io_connect_async_method as the MIG server runs it, `method` being the user client's externalMethod body
template <typename Method>
IOReturn asyncCall(Method method, uint32_t referenceCount = kOSAsyncRef64Count)
{
	gRights.make(kPort);
	io_user_reference_t reference[kOSAsyncRef64Count] = {kPort | kIOUCAsync64Flag, 0x1000,
							     0x2000};
	IOExternalMethodArguments arguments = {};
	arguments.asyncReference = reference;
	arguments.asyncReferenceCount = referenceCount;
	arguments.asyncWakePort = kPort;
	SwitchGpuAsyncReference held(&arguments);
	const IOReturn result = held.settle(method(&arguments, held));
	if (result != kIOReturnSuccess)
		gRights.release(kPort);
	return result;
}

class AsyncRights : public ::testing::Test
{
protected:
	void SetUp() override
	{
		gRights.reset();
		client_ = new AGXDeviceUserClient;
		client_->bos_.push_back({7, reinterpret_cast<switch_gpu_backing *>(0x70), nullptr,
					 nullptr, 0, false, nullptr});
	}

	void TearDown() override
	{
		client_->release();
		EXPECT_EQ(gRights.underflows, 0u) << "a send right was released twice";
		EXPECT_EQ(gRights.orphanSends, 0u)
			<< "a notification went to a port the kernel held no right to";
		EXPECT_EQ(gRights.held(), 0) << "a send right was never released";
	}

	IOReturn observeEvent(uint32_t flags = 0, uint32_t referenceCount = kOSAsyncRef64Count)
	{
		const TegraGM20BAbiEventObserve in = {3, 1, 11, flags, 0};
		return asyncCall(
			[&](IOExternalMethodArguments *arguments, SwitchGpuAsyncReference &held) {
				return client_->eventObserve(&in, arguments, held);
			},
			referenceCount);
	}

	IOReturn observeResource(uint64_t handle)
	{
		const TegraGM20BAbiSurfaceUse use = {handle, TegraGM20BSurfaceRead, 0};
		const TegraGM20BAbiResourceObserve in = {12, uint64_t(uintptr_t(&use)), 1, 0, 0};
		return asyncCall(
			[&](IOExternalMethodArguments *arguments, SwitchGpuAsyncReference &held) {
				return client_->resourceObserve(&in, arguments, held);
			});
	}

	AGXDeviceUserClient *client_ = nullptr;
};

TEST_F(AsyncRights, AFailedCallLeavesTheRightToMig)
{
	EXPECT_EQ(asyncCall([](IOExternalMethodArguments *, SwitchGpuAsyncReference &) {
			  return kIOReturnNotReady;
		  }),
		  kIOReturnNotReady);
	EXPECT_EQ(gRights.rights[kPort], 0);
}

TEST_F(AsyncRights, ASuccessfulCallThatKeepsNothingReleasesTheRight)
{
	EXPECT_EQ(asyncCall([](IOExternalMethodArguments *, SwitchGpuAsyncReference &) {
			  return kIOReturnSuccess;
		  }),
		  kIOReturnSuccess);
	EXPECT_EQ(gRights.rights[kPort], 0);
}

TEST_F(AsyncRights, ATakenRightIsTheHoldersToRelease)
{
	OSAsyncReference64 kept = {};
	EXPECT_EQ(asyncCall([&](IOExternalMethodArguments *, SwitchGpuAsyncReference &held) {
			  held.take(kept);
			  return kIOReturnSuccess;
		  }),
		  kIOReturnSuccess);
	EXPECT_EQ(gRights.rights[kPort], 1);
	IOUserClient::releaseAsyncReference64(kept);
}

TEST_F(AsyncRights, ARefusedEventObserveLeavesTheRightToMig)
{
	client_->events_.refuse = -28;
	EXPECT_EQ(observeEvent(), kIOReturnNoResources);
	EXPECT_EQ(observeEvent(TegraGM20BEventObserveProbe), kIOReturnBadArgument);
	EXPECT_EQ(observeEvent(0, 2), kIOReturnBadArgument);
}

TEST_F(AsyncRights, AnArmedEventObserveReleasesTheRightWhenItFires)
{
	ASSERT_EQ(observeEvent(), kIOReturnSuccess);
	EXPECT_EQ(gRights.rights[kPort], 1);
	client_->events_.fire(0);
	ASSERT_EQ(gRights.replies.size(), 1u);
	EXPECT_EQ(gRights.replies[0].result, kIOReturnSuccess);
	EXPECT_EQ(gRights.rights[kPort], 0);
}

TEST_F(AsyncRights, ARefusedResourceObserveLeavesTheRightToMig)
{
	client_->resources_.refuse = -28;
	EXPECT_EQ(observeResource(7), kIOReturnNoResources);
	client_->resources_.refuse = 0;
	EXPECT_EQ(observeResource(8), kIOReturnBadArgument);
	client_->submitGate_.shut = true;
	EXPECT_EQ(observeResource(7), kIOReturnNotOpen);
}

TEST_F(AsyncRights, AnArmedResourceObserveReleasesTheRightWhenItFires)
{
	ASSERT_EQ(observeResource(7), kIOReturnSuccess);
	EXPECT_EQ(gRights.rights[kPort], 1);
	client_->resources_.fire(-5);
	ASSERT_EQ(gRights.replies.size(), 1u);
	EXPECT_EQ(gRights.replies[0].result, kIOReturnAborted);
	EXPECT_EQ(gRights.rights[kPort], 0);
}

}  // namespace
