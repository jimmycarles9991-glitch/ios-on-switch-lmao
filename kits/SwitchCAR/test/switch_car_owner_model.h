// The CAR and PMC as the kit's owner tests see them: a register file on a clock in nanoseconds (each register access
// takes 100 ns, IODelay the time asked), a second driver's step that lands at the first read of a chosen register, and
// the PMC's deep power-down state machine (TRM 12.6.111): a request written to IO_DPD_REQ or IO_DPD2_REQ starts an
// operation whose pads change SEL_DPD_TIM APB clocks later; the machine is busy for five clocks more, and a request
// written while it is busy only updates the register.
#ifndef SWITCH_TEST_SWITCH_CAR_OWNER_MODEL_H
#define SWITCH_TEST_SWITCH_CAR_OWNER_MODEL_H

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace switch_car_owner_model {
constexpr uintptr_t kCar = 0x60006000, kPmc = 0x7000e400;
constexpr uint32_t kDpdReq = 0x1b8, kDpdStatus = 0x1bc, kDpd2Req = 0x1c0, kDpd2Status = 0x1c4, kSelDpdTim = 0x1c8;

void reset();
uint32_t &reg(uintptr_t address);
std::vector<std::pair<uintptr_t, uint32_t>> writes();

// `step` runs at the first read of `address` by anyone: at once if the kit's lock is free, else right after its
// release, as a second thread blocked on the lock would.
void landsAtTheNextRead(uintptr_t address, std::function<void()> step);
bool secondRan();

// The APB clock, for the deep power-down machine's SEL_DPD_TIM.
void apbKHz(uint32_t khz);
unsigned droppedDpdRequests();
}  // namespace switch_car_owner_model

#endif
