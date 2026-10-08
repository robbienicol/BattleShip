#pragma once

/* Rollback netplay session (GekkoNet). Work in progress: not wired up yet. */

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the VS battle scene once a match is set up. */
void port_rollback_on_battle_start(void);

/* Runs this frame's ticks when a rollback session is active; returns
 * non-zero if it did (the caller then skips its normal tick). */
int port_rollback_session_frame(void);

#ifdef __cplusplus
}
#endif
