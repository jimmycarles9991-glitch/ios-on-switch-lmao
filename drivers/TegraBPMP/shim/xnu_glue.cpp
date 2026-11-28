// SPDX-License-Identifier: BSD-2-Clause-Patent
// XNU side of the TegraBPMP shim (bpxnu.h): the T210 BPMP doorbells that L4T's drivers/irqchip/irq-tegra.c provides
// (tegra_ring_doorbell / tegra_register_doorbell_handler, soc/tegra/doorbell.h).
//
// Doorbells (irq-tegra.c tegra_ictlr_init + l4t-hardware-soc-t210 tegra210-soc-base.dtsi lic node):
//   * inbound, BPMP -> CPU: the ICTLR node's `interrupts = <0 4 4  0 5 4  0 7 4  0 18 4>`, i.e. GIC SPIs 4, 5, 7, 18
//     (INTID 36, 37, 39, 50), level high, doorbell 0..3 in that order; mail_t210.c registers doorbell i as inbound
//     channel index i (bpmp_init_irq). The BPMP raises one by setting its bit in the legacy interrupt controller's
//     forced-interrupt register (LIC bank 0 CPU_IEP_FIR, 0x60004014); the CPU handler clears it through FIR_CLR
//     (0x6000401c) first, as irq-tegra.c's doorbell_handler -> ack_doorbell does, then runs mail.c's handler.
//   * outbound, CPU -> BPMP: `outgoing-doorbell = <6>`: FIR_SET bit 6 (tegra_ring_doorbell, CPU_OB_DOORBELL 4).
// Each inbound doorbell is an IOFilterInterruptEventSource on this service's work loop: the filter (primary
// interrupt context) clears the FIR bit and runs mail.c's bpmp_handle_irq() directly, avoiding an extra prio-81
// work-loop hop (mail.c, mailman.c, and shim/linux.c synchronize thread paths with spin_lock_irqsave, and
// complete()/up() only wake waiters via thread_wakeup_one). ArmGIC gates the LIC CPU_IER bit with the SPI. A doorbell
// that cannot be claimed panics; one that never comes for an answered request panics from mail.c (bpxnu_doorbell_lost).
// No polled mode.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include "../../ArmGIC/SwitchInterrupts.h"
#include "include/bpxnu.h"
#include "../../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace
{
constexpr vm_offset_t kLicPA = 0x60004000;

enum : uint32_t {
	ICTLR_CPU_IEP_FIR = 0x14,
	ICTLR_CPU_IEP_FIR_SET = 0x18,
	ICTLR_CPU_IEP_FIR_CLR = 0x1c,
	ICTLR_CPU_IER = 0x20
};

constexpr unsigned kDoorbells = 4;
constexpr uint32_t kInboundSpi[kDoorbells] = {4, 5, 7, 18};
constexpr uint32_t kOutboundBit = 6;
constexpr unsigned kOutboundDoorbell = 4;  // mail_t210.c CPU_OB_DOORBELL

volatile uint8_t *gLic;

struct Doorbell {
	void (*handler)(void *data);
	void *data;
};

Doorbell gDoorbells[kDoorbells];

volatile uint8_t *lic()
{
	if (!gLic)
		gLic = switch_mmio_map(kLicPA, 0x1000);
	return gLic;
}

inline uint32_t licRead(uint32_t offset)
{
	return switch_mmio_read32(lic(), offset);
}

inline void licWrite(uint32_t offset, uint32_t value)
{
	switch_mmio_write32(lic(), offset, value);
}
// NOLINTNEXTLINE(misc-const-correctness): IOInterruptAction's signature
void doorbellInterrupt(OSObject *, void *refCon, IOService *, int)
{
	const unsigned i = static_cast<unsigned>(reinterpret_cast<uintptr_t>(refCon));
	licWrite(ICTLR_CPU_IEP_FIR_CLR, 1u << kInboundSpi[i]);
	(void)licRead(ICTLR_CPU_IEP_FIR);
	if (auto *handler = __atomic_load_n(&gDoorbells[i].handler, __ATOMIC_ACQUIRE))
		handler(gDoorbells[i].data);
}
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class TegraBPMPDoorbells : public IOService
{
	// NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
	OSDeclareDefaultStructors(TegraBPMPDoorbells)

public:
	bool claim();
};

OSDefineMetaClassAndStructors(TegraBPMPDoorbells, IOService)

bool TegraBPMPDoorbells::claim()
{
	IOService *nubs[kDoorbells] = {};
	for (unsigned i = 0; i < kDoorbells; ++i) {
		const uint32_t intid = 32 + kInboundSpi[i];
		IOService *nub = switch_interrupt_nub(intid, false);
		// NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): ArmGIC owns its interrupt nubs
		if (!nub || nub->registerInterrupt(0, nullptr, &doorbellInterrupt,
						   reinterpret_cast<void *>(uintptr_t{i})) != kIOReturnSuccess) {
			panic("TegraBPMP: cannot attach BPMP doorbell %u (INTID %u)", i, intid);
		}
		nubs[i] = nub;
	}
	// A doorbell the BPMP forced before this point is not cleared: it fires as soon as its source is enabled and
	// mail.c then answers whatever inbound mail it signalled, as Linux does after request_irq().
	for (unsigned i = 0; i < kDoorbells; ++i) {
		if (nubs[i]->enableInterrupt(0) != kIOReturnSuccess)
			panic("TegraBPMP: cannot enable BPMP doorbell %u (INTID %u)", i, 32 + kInboundSpi[i]);
	}
	return true;
}

extern "C" {

int tegra_register_doorbell_handler(unsigned int doorbell_id, void (*handler)(void *data),
				    void *data)
{
	if (doorbell_id >= kDoorbells)
		return -22;  // -EINVAL, as irq-tegra.c
	gDoorbells[doorbell_id].data = data;
	__atomic_store_n(&gDoorbells[doorbell_id].handler, handler, __ATOMIC_RELEASE);
	return 0;
}

int tegra_ring_doorbell(unsigned int doorbell_id)
{
	if (doorbell_id != kOutboundDoorbell)
		return -22;
	__asm__ volatile("dsb st" ::
				 : "memory");  // the channel area writes land before the BPMP looks
	licWrite(ICTLR_CPU_IEP_FIR_SET, 1u << kOutboundBit);
	return 0;
}

void bpxnu_doorbells_start(void)
{
	static TegraBPMPDoorbells *service;
	if (service)
		return;
	IOService *resources = IOService::getResourceService();
	TegraBPMPDoorbells *candidate = OSTypeAlloc(TegraBPMPDoorbells);
	if (!lic() || !resources || !candidate || !candidate->init() ||
	    !candidate->attach(resources) || !candidate->start(resources)) {
		panic("TegraBPMP: cannot create the BPMP doorbell service");
	}
	if (!candidate->claim())
		panic("TegraBPMP: no work loop for the BPMP doorbells");
	candidate->setName("TegraBPMP");
	candidate->registerService();
	service = candidate;
}

void bpxnu_doorbell_lost(int ch, int mrq)
{
	const uint32_t fir = licRead(ICTLR_CPU_IEP_FIR);
	const uint32_t ier = licRead(ICTLR_CPU_IER);
	uint32_t spi = kInboundSpi[0];
	for (const uint32_t inbound : kInboundSpi) {
		if (fir & (1u << inbound)) {
			spi = inbound;
			break;
		}
	}
	char gic[160];
	switch_interrupt_describe(32 + spi, gic, sizeof(gic));
	panic("TegraBPMP: doorbell lost: BPMP answered mrq %d on ch %d, no interrupt in 4 s; LIC "
	      "FIR %08x IER %08x; %s",
	      mrq, ch, fir, ier, gic);
}

}  // extern "C"
