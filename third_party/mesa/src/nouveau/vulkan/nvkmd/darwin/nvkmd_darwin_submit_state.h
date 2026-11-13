/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_DARWIN_SUBMIT_STATE_H
#define NVKMD_DARWIN_SUBMIT_STATE_H
#include <stdbool.h>
#include <stdint.h>

/* Submission failure outside the kernel's channel-death codes 1..4. */
#define DARWIN_SUBMIT_REJECTED UINT32_C(0x80000001)

struct darwin_submit_state {
   uint32_t dead;
   uint32_t refused;
};

struct darwin_submit_completion {
   uint32_t error;
   uint32_t seqno;
   bool pending;
};

static inline uint32_t
darwin_submit_blocked(const struct darwin_submit_state *state)
{
   return state->refused ? state->refused : state->dead;
}

static inline void
darwin_submit_reject(struct darwin_submit_state *state, uint32_t confirmed_dead)
{
   if (confirmed_dead)
      state->dead = confirmed_dead;
   if (!state->refused)
      state->refused = confirmed_dead ? confirmed_dead : DARWIN_SUBMIT_REJECTED;
}

static inline struct darwin_submit_completion
darwin_submit_finish(struct darwin_submit_state *state, bool submitted,
                     uint32_t seqno)
{
   const struct darwin_submit_completion result = {
      darwin_submit_blocked(state), seqno, submitted,
   };
   state->refused = 0;
   return result;
}

#endif
