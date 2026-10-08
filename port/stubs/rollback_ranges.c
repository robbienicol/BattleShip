/**
 * rollback_ranges.c — memory ranges the rollback snapshot handles specially.
 *
 * Compiled with the decomp headers (it is part of the ssb64_game target) so
 * every size is sizeof() of the real declaration.
 */
#include <sys/controller.h>
#include <sys/malloc.h>
#include <sys/netinput.h>

#include "../rollback/rollback_ranges.h"

/* Gameplay values in files whose globals are otherwise never restored. */
extern u32 sSYSchedulerTicCount;   /* match timer counts scheduler tics */
extern u32 dSYTaskmanUpdateCount;
extern SYMallocRegion gSYTaskmanGeneralHeap; /* scene arena bump pointer */
extern SYController gSYControllerDevices[MAXCONTROLLERS];
extern SYController gSYControllerMain;

const PortRollbackRange gPortRollbackExtras[] = {
	{ &sSYSchedulerTicCount, sizeof(sSYSchedulerTicCount) },
	{ &dSYTaskmanUpdateCount, sizeof(dSYTaskmanUpdateCount) },
	{ &gSYTaskmanGeneralHeap, sizeof(gSYTaskmanGeneralHeap) },
	{ gSYControllerDevices, sizeof(gSYControllerDevices) },
	{ &gSYControllerMain, sizeof(gSYControllerMain) },
};
const int gPortRollbackExtrasCount = sizeof(gPortRollbackExtras) / sizeof(gPortRollbackExtras[0]);

/* Input rings and replay storage: confirmed remote/saved inputs must survive
 * a rollback, and the resolved ring is rewritten by re-simulation anyway. */
extern SYNetInputFrame sSYNetInputHistory[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
extern SYNetInputFrame sSYNetInputRemoteHistory[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
extern SYNetInputFrame sSYNetInputSavedHistory[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
extern SYNetInputFrame sSYNetInputReplayFrames[MAXCONTROLLERS][SYNETINPUT_REPLAY_MAX_FRAMES];

const PortRollbackRange gPortRollbackExcludes[] = {
	{ sSYNetInputHistory, sizeof(sSYNetInputHistory) },
	{ sSYNetInputRemoteHistory, sizeof(sSYNetInputRemoteHistory) },
	{ sSYNetInputSavedHistory, sizeof(sSYNetInputSavedHistory) },
	{ sSYNetInputReplayFrames, sizeof(sSYNetInputReplayFrames) },
};
const int gPortRollbackExcludesCount = sizeof(gPortRollbackExcludes) / sizeof(gPortRollbackExcludes[0]);
