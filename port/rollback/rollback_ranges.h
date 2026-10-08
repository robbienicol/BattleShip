#pragma once

/* Memory ranges the rollback snapshot treats specially. Defined in
 * port/stubs/rollback_ranges.c, which is compiled with the decomp headers so
 * the sizes come from the real types. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortRollbackRange {
	void *base;
	unsigned long size;
} PortRollbackRange;

/* Gameplay values that live in engine-plumbing files (excluded sections). */
extern const PortRollbackRange gPortRollbackExtras[];
extern const int gPortRollbackExtrasCount;

/* Large buffers inside the snapshot sections that must not be restored. */
extern const PortRollbackRange gPortRollbackExcludes[];
extern const int gPortRollbackExcludesCount;

#ifdef __cplusplus
}
#endif
