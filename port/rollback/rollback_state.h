#pragma once

/**
 * rollback_state.h — saving and restoring the VS simulation for rollback.
 */

#ifdef __cplusplus
#include "rollback_io.h"

/* Serializes the simulation at a tick boundary. False outside a scene. */
bool PortRollbackSave(RollbackBuffer &out);

/* Restores a state from PortRollbackSave taken in the same scene. */
bool PortRollbackLoad(const uint8_t *data, size_t len);

extern "C" {
#endif

/* Test harness: SSB64_SYNCTEST=K rolls back K ticks every tick and checks
 * the re-simulation matches. */
void port_rollback_synctest_tick(void);

/* Diagnostics: SSB64_ROLLBACK_PROBE=1 logs snapshot size and churn. */
void port_rollback_probe_tick(void);

#ifdef __cplusplus
}
#endif
