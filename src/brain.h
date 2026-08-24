#ifndef GG_CQB_BRAIN_H
#define GG_CQB_BRAIN_H

#include "sim.h"

#if defined(__SDCC)
#include <gbdk/platform.h>
#else
#ifndef BANKED
#define BANKED
#endif
#endif

void brain_act_agent(Sim *s, uint8_t id) BANKED;
void brain_run_actor_round(Sim *s, uint8_t first_team, uint8_t slots) BANKED;

#endif
