#ifndef PD_JIT_TEST_H
#define PD_JIT_TEST_H

#include "pd_api.h"

// One-shot dynarec feasibility probe: generates Thumb-2 code at runtime,
// flushes caches, executes it, and logs whether RWX works on this device.
// No-op on the simulator.
void pd_jit_test(PlaydateAPI *pd);

#endif
