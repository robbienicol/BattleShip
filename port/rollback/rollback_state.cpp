/**
 * rollback_state.cpp — saving and restoring the VS simulation for rollback.
 *
 * The simulation lives in:
 *   - decomp globals, grouped into the __ssbdata/__ssbbss sections by
 *     port/rollback/sections.h (engine plumbing is compiled into separate,
 *     never-restored sections; see rollback_ranges.c for the exceptions);
 *   - the scene arena from gPortArenaSimStart up to the general heap's bump
 *     pointer (everything below it is the render pipeline);
 *   - pooled GObj-thread coroutines (countdown, announcer, ...);
 *   - port-side bookkeeping for in-place data fixups (port_aobj_fixup).
 */

#include "rollback_state.h"
#include "rollback_io.h"
#include "rollback_ranges.h"
#include "coroutine.h"
#include "port_log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#endif

extern "C" {

#if defined(__APPLE__)
extern char sRollbackDataStart __asm("section$start$__DATA$__ssbdata");
extern char sRollbackDataEnd __asm("section$end$__DATA$__ssbdata");
extern char sRollbackBssStart __asm("section$start$__DATA$__ssbbss");
extern char sRollbackBssEnd __asm("section$end$__DATA$__ssbbss");
#endif

/* Mirrors SYMallocRegion (decomp/src/sys/malloc.h). */
struct PortMallocRegion {
	uint32_t id;
	void *start;
	void *end;
	void *ptr;
};
extern PortMallocRegion gSYTaskmanGeneralHeap;
extern void *gPortArenaSimStart;
extern unsigned int syNetInputGetTick(void);
extern unsigned char gSCManagerSceneData; /* first byte is scene_curr */

/* Mirrors SYNetSyncTickHash (decomp/src/sys/netsync.h). */
enum { kSyncColumns = 12, kSyncGatedMask = (1u << 8) - 1 };
struct PortSyncTickHash {
	uint32_t column[kSyncColumns];
};
extern void syNetSyncHashTick(PortSyncTickHash *out);
extern const char *syNetSyncGetColumnName(int column);

extern void PortRunHeadlessTick(void);

} /* extern "C" */

namespace {

constexpr uint32_t kMagic = 0x52424B31; /* 'RBK1' */
constexpr unsigned char kSceneVSBattle = 22;

struct Range {
	uint8_t *base;
	size_t size;
	const char *name;
};

/* The snapshot sections minus the excluded buffers, computed once. */
const std::vector<Range> &SectionRanges()
{
	static std::vector<Range> sRanges;
	static bool sBuilt = false;
	if (sBuilt) {
		return sRanges;
	}
	sBuilt = true;
	std::vector<Range> whole;
#if defined(__APPLE__)
	whole.push_back({ (uint8_t *)&sRollbackDataStart, (size_t)(&sRollbackDataEnd - &sRollbackDataStart), "data" });
	whole.push_back({ (uint8_t *)&sRollbackBssStart, (size_t)(&sRollbackBssEnd - &sRollbackBssStart), "bss" });
#endif
	std::vector<Range> excludes;
	for (int i = 0; i < gPortRollbackExcludesCount; i++) {
		excludes.push_back({ (uint8_t *)gPortRollbackExcludes[i].base, gPortRollbackExcludes[i].size, "exclude" });
	}
	std::sort(excludes.begin(), excludes.end(), [](const Range &a, const Range &b) { return a.base < b.base; });

	for (const Range &r : whole) {
		uint8_t *at = r.base;
		uint8_t *end = r.base + r.size;
		for (const Range &x : excludes) {
			uint8_t *xs = x.base, *xe = x.base + x.size;
			if (xe <= at || xs >= end) {
				continue;
			}
			if (xs > at) {
				sRanges.push_back({ at, (size_t)(xs - at), r.name });
			}
			at = std::max(at, xe);
		}
		if (at < end) {
			sRanges.push_back({ at, (size_t)(end - at), r.name });
		}
	}
	size_t total = 0;
	for (const Range &r : sRanges) total += r.size;
	port_log("SSB64 Rollback: %zu section ranges, %zu KiB of game globals\n", sRanges.size(), total / 1024);
	return sRanges;
}

bool AddrInSections(uintptr_t addr)
{
	for (const Range &r : SectionRanges()) {
		if (addr >= (uintptr_t)r.base && addr < (uintptr_t)r.base + r.size) return true;
	}
	return false;
}

bool InVSBattle()
{
	return gSCManagerSceneData == kSceneVSBattle && gPortArenaSimStart != nullptr;
}

} /* namespace */

/* ========================================================================= */
/*  Save / load                                                              */
/* ========================================================================= */

extern "C" int port_rollback_addr_restored(uintptr_t addr)
{
	if (gPortArenaSimStart != nullptr && addr >= (uintptr_t)gPortArenaSimStart &&
	    addr < (uintptr_t)gSYTaskmanGeneralHeap.end) {
		return 1;
	}
	return AddrInSections(addr) ? 1 : 0;
}

bool PortRollbackSave(RollbackBuffer &out)
{
	using Clock = std::chrono::steady_clock;
	static int sProfile = -1;
	if (sProfile < 0) {
		const char *env = std::getenv("SSB64_ROLLBACK_PROFILE");
		sProfile = (env != nullptr) ? std::atoi(env) : 0;
	}
	Clock::time_point marks[8];
	int nmarks = 0;
	auto mark = [&]() {
		if (sProfile > 0 && nmarks < 8) marks[nmarks++] = Clock::now();
	};
	mark();
	out.clear();
	if (gPortArenaSimStart == nullptr) {
		return false;
	}
	RollbackWriter w{ out };
	w.value(kMagic);
	w.value<uint64_t>((uintptr_t)gPortArenaSimStart);

	for (const Range &r : SectionRanges()) {
		w.bytes(r.base, r.size);
	}
	for (int i = 0; i < gPortRollbackExtrasCount; i++) {
		w.bytes(gPortRollbackExtras[i].base, gPortRollbackExtras[i].size);
	}
	mark();

	uint8_t *arena = (uint8_t *)gPortArenaSimStart;
	uint64_t arena_len = (uint64_t)((uint8_t *)gSYTaskmanGeneralHeap.ptr - arena);
	w.value(arena_len);
	w.bytes(arena, (size_t)arena_len);

	mark();
	/* Pooled coroutines, length-prefixed. Grow the buffer until they fit. */
	size_t len_at = out.size;
	out.grow(sizeof(uint64_t));
	size_t pool_at = out.size;
	uint64_t pool_len = 0;
	for (size_t cap = 256 * 1024; pool_len == 0; cap *= 4) {
		if (cap > 64 * 1024 * 1024) {
			return false; /* unsupported on this platform */
		}
		out.size = pool_at;
		pool_len = port_coroutine_pool_save(out.grow(cap), cap);
	}
	out.size = pool_at + pool_len;
	std::memcpy(out.data + len_at, &pool_len, sizeof(uint64_t));

	mark();
	port_aobj_fixup_save(w);
	port_lbreloc_save(w);
	port_byteswap_fixups_save(w);
	mark();
	if (sProfile > 0) {
		/* Baseline: raw memcpy of the same ranges into a reused buffer. */
		static std::vector<uint8_t> sScratch(8 * 1024 * 1024);
		auto c0 = Clock::now();
		size_t at = 0;
		for (const Range &r : SectionRanges()) {
			std::memcpy(sScratch.data() + at, r.base, r.size);
			at += r.size;
		}
		auto c1 = Clock::now();
		std::memcpy(sScratch.data() + at, gPortArenaSimStart,
		            (size_t)((uint8_t *)gSYTaskmanGeneralHeap.ptr - (uint8_t *)gPortArenaSimStart));
		auto c2 = Clock::now();
		port_log("SSB64 Rollback: raw memcpy sections=%ldus arena=%ldus\n",
		         (long)std::chrono::duration_cast<std::chrono::microseconds>(c1 - c0).count(),
		         (long)std::chrono::duration_cast<std::chrono::microseconds>(c2 - c1).count());
		sProfile--;
		auto us = [&](int a, int b) {
			return (long)std::chrono::duration_cast<std::chrono::microseconds>(marks[b] - marks[a]).count();
		};
		port_log("SSB64 Rollback: save profile sections=%ldus arena=%ldus pool=%ldus port=%ldus total=%zu KiB\n",
		         us(0, 1), us(1, 2), us(2, 3), us(3, 4), out.size / 1024);
	}
	return true;
}

bool PortRollbackLoad(const uint8_t *data, size_t len)
{
	RollbackReader r{ data, data + len };
	uint32_t magic = 0;
	uint64_t sim_start = 0;
	if (!r.value(magic) || magic != kMagic || !r.value(sim_start) ||
	    sim_start != (uintptr_t)gPortArenaSimStart) {
		port_log("SSB64 Rollback: refusing to load a snapshot from another scene\n");
		return false;
	}
	/* The arena tail above the restored bump pointer must read as fresh
	 * (zeroed) memory, as it would on first allocation. */
	uint8_t *cur_ptr = (uint8_t *)gSYTaskmanGeneralHeap.ptr;

	for (const Range &range : SectionRanges()) {
		r.bytes(range.base, range.size);
	}
	for (int i = 0; i < gPortRollbackExtrasCount; i++) {
		r.bytes(gPortRollbackExtras[i].base, gPortRollbackExtras[i].size);
	}
	uint64_t arena_len = 0;
	r.value(arena_len);
	uint8_t *arena = (uint8_t *)gPortArenaSimStart;
	r.bytes(arena, (size_t)arena_len);
	if (cur_ptr > arena + arena_len) {
		std::memset(arena + arena_len, 0, (size_t)(cur_ptr - (arena + arena_len)));
	}

	uint64_t pool_len = 0;
	r.value(pool_len);
	if (!r.ok || (size_t)(r.end - r.at) < pool_len || !port_coroutine_pool_load(r.at, (size_t)pool_len)) {
		port_log("SSB64 Rollback: coroutine pool restore failed\n");
		return false;
	}
	r.at += pool_len;

	if (!port_aobj_fixup_load(r) || !port_lbreloc_load(r) || !port_byteswap_fixups_load(r) || !r.ok) {
		port_log("SSB64 Rollback: snapshot truncated\n");
		return false;
	}
	return true;
}

/* ========================================================================= */
/*  Sync test                                                                */
/* ========================================================================= */

namespace {

struct SyncEntry {
	unsigned tick = ~0u;
	RollbackBuffer state;
	PortSyncTickHash hash{};
};

/* Names the memory a snapshot byte offset falls in, for divergence reports. */
void DescribeOffset(const RollbackBuffer &state, size_t offset, char *buf, size_t buf_len)
{
	size_t at = sizeof(uint32_t) + sizeof(uint64_t);
	for (const Range &range : SectionRanges()) {
		if (offset < at + range.size) {
			void *addr = range.base + (offset - at);
#if defined(__APPLE__) || defined(__linux__)
			Dl_info info;
			if (dladdr(addr, &info) != 0 && info.dli_sname != nullptr) {
				snprintf(buf, buf_len, "%s %s+0x%zx", range.name, info.dli_sname,
				         (size_t)((uint8_t *)addr - (uint8_t *)info.dli_saddr));
				return;
			}
#endif
			snprintf(buf, buf_len, "%s %p", range.name, addr);
			return;
		}
		at += range.size;
	}
	for (int i = 0; i < gPortRollbackExtrasCount; i++) {
		if (offset < at + gPortRollbackExtras[i].size) {
			snprintf(buf, buf_len, "extra[%d]+0x%zx", i, offset - at);
			return;
		}
		at += gPortRollbackExtras[i].size;
	}
	uint64_t arena_len = 0;
	if (at + sizeof(arena_len) <= state.size) {
		std::memcpy(&arena_len, state.data + at, sizeof(arena_len));
	}
	at += sizeof(arena_len);
	if (offset < at + arena_len) {
		snprintf(buf, buf_len, "arena+0x%zx", offset - at);
		return;
	}
	snprintf(buf, buf_len, "pool/port+0x%zx", offset - at - (size_t)arena_len);
}

} /* namespace */

/* SSB64_SYNCTEST=K: every VS tick, save the state; then load the state from
 * K ticks ago and re-simulate those K ticks headless, checking each one's
 * gameplay hash against the original. SSB64_SYNCTEST_STRICT=1 also compares
 * the full snapshot bytes after re-simulating. */
extern "C" void port_rollback_synctest_tick(void)
{
	static int sDepth = -1;
	static bool sStrict = false;
	if (sDepth < 0) {
		const char *env = std::getenv("SSB64_SYNCTEST");
		sDepth = (env != nullptr) ? std::atoi(env) : 0;
		const char *strict = std::getenv("SSB64_SYNCTEST_STRICT");
		sStrict = (strict != nullptr && strict[0] == '1');
		if (sDepth > 0) {
			port_log("SSB64 SyncTest: enabled depth=%d strict=%d\n", sDepth, sStrict ? 1 : 0);
		}
	}
	if (sDepth <= 0) {
		return;
	}
	static std::vector<SyncEntry> sRing;
	static unsigned sLastTick = ~0u;
	static unsigned sChecked = 0, sHashFails = 0, sByteFails = 0;
	static double sSaveMs = 0, sLoadMs = 0, sResimMs = 0;
	static unsigned sSamples = 0;

	if (!InVSBattle()) {
		if (!sRing.empty()) {
			port_log("SSB64 SyncTest: battle over — checked=%u hash_mismatches=%u byte_mismatches=%u "
			         "avg save=%.3fms load=%.3fms resim(%d)=%.2fms\n",
			         sChecked, sHashFails, sByteFails, sSamples ? sSaveMs / sSamples : 0.0,
			         sSamples ? sLoadMs / sSamples : 0.0, sDepth, sSamples ? sResimMs / sSamples : 0.0);
			sRing.clear();
		}
		sLastTick = ~0u;
		return;
	}
	if (sRing.empty()) {
		sRing.resize((size_t)sDepth + 1);
	}
	unsigned tick = syNetInputGetTick();
	if (tick == sLastTick) {
		return;
	}
	sLastTick = tick;

	using Clock = std::chrono::steady_clock;
	auto ms = [](Clock::time_point a, Clock::time_point b) {
		return std::chrono::duration<double, std::milli>(b - a).count();
	};

	SyncEntry &now = sRing[tick % sRing.size()];
	auto t0 = Clock::now();
	if (!PortRollbackSave(now.state)) {
		port_log("SSB64 SyncTest: save failed at tick %u\n", tick);
		return;
	}
	auto t1 = Clock::now();
	now.tick = tick;
	syNetSyncHashTick(&now.hash);

	if (tick < (unsigned)sDepth) {
		return;
	}
	SyncEntry &from = sRing[(tick - sDepth) % sRing.size()];
	if (from.tick != tick - (unsigned)sDepth) {
		return;
	}
	static RollbackBuffer original;
	if (sStrict) {
		original.assign(now.state);
	}
	auto t2 = Clock::now();
	if (!PortRollbackLoad(from.state.data, from.state.size)) {
		return;
	}
	auto t3 = Clock::now();
	for (int i = 0; i < sDepth; i++) {
		PortRunHeadlessTick();
		unsigned t = syNetInputGetTick();
		SyncEntry &orig = sRing[t % sRing.size()];
		if (orig.tick != t) {
			port_log("SSB64 SyncTest: tick %u re-simulated as %u (expected %u)\n", tick, t, from.tick + i + 1);
			continue;
		}
		PortSyncTickHash h;
		syNetSyncHashTick(&h);
		/* Re-save, as a rollback session does for every re-simulated frame:
		 * later rollbacks restore these states, not the discarded ones. */
		PortRollbackSave(orig.state);
		uint32_t mismatch = 0;
		for (int c = 0; c < kSyncColumns; c++) {
			if (h.column[c] != orig.hash.column[c]) mismatch |= 1u << c;
		}
		sChecked++;
		if (mismatch & kSyncGatedMask) {
			if (sHashFails < 20) {
				char cols[256] = { 0 };
				for (int c = 0; c < kSyncColumns; c++) {
					if (mismatch & (1u << c)) {
						strncat(cols, syNetSyncGetColumnName(c), sizeof(cols) - strlen(cols) - 2);
						strncat(cols, " ", sizeof(cols) - strlen(cols) - 1);
					}
				}
				port_log("SSB64 SyncTest: DESYNC rolling back %u->%u, re-simulated tick %u differs in: %s\n",
				         from.tick, tick, t, cols);
			}
			sHashFails++;
		}
	}
	auto t4 = Clock::now();
	sSaveMs += ms(t0, t1);
	sLoadMs += ms(t2, t3);
	sResimMs += ms(t3, t4);
	sSamples++;

	if (sStrict) {
		const RollbackBuffer &again = now.state;
		size_t n = std::min(again.size, original.size);
		size_t first = n, diffs = 0;
		for (size_t i = 0; i < n; i++) {
			if (again.data[i] != original.data[i]) {
				if (first == n) first = i;
				diffs++;
			}
		}
		if ((diffs != 0 || again.size != original.size) && sByteFails == 0) {
			/* First time: list every differing symbol once. */
			char last[256] = { 0 };
			int listed = 0;
			for (size_t i = 0; i < n && listed < 40; i += 4) {
				if (std::memcmp(again.data + i, original.data + i, std::min<size_t>(4, n - i)) == 0) continue;
				char where[256];
				DescribeOffset(original, i, where, sizeof(where));
				char *plus = std::strchr(where, '+');
				if (plus) *plus = 0;
				if (std::strcmp(where, last) != 0) {
					port_log("SSB64 SyncTest:   differs: %s\n", where);
					std::snprintf(last, sizeof(last), "%s", where);
					listed++;
				}
			}
		}
		if (diffs != 0 || again.size != original.size) {
			if (sByteFails < 20) {
				char where[256];
				DescribeOffset(original, first, where, sizeof(where));
				port_log("SSB64 SyncTest: state bytes differ after re-simulating to tick %u: %zu bytes, "
				         "first at %s (sizes %zu vs %zu)\n",
				         tick, diffs, where, again.size, original.size);
			}
			sByteFails++;
		}
	}
	if (tick % 600 == 0) {
		port_log("SSB64 SyncTest: tick=%u checked=%u hash_mismatches=%u byte_mismatches=%u state=%zu KiB "
		         "save=%.3fms load=%.3fms resim=%.2fms\n",
		         tick, sChecked, sHashFails, sByteFails, now.state.size / 1024, ms(t0, t1), ms(t2, t3),
		         ms(t3, t4));
	}
}

/* SSB64_ROLLBACK_PROBE=1: during VS battles, log how much simulation memory
 * exists and how many 4 KiB pages of it change in one tick and in 60 ticks. */
extern "C" void port_rollback_probe_tick(void)
{
	static int sEnabled = -1;
	if (sEnabled < 0) {
		const char *env = std::getenv("SSB64_ROLLBACK_PROBE");
		sEnabled = (env != nullptr && env[0] == '1') ? 1 : 0;
	}
	if (!sEnabled || !InVSBattle()) {
		return;
	}
	static RollbackBuffer sBaseline;
	static unsigned sBaselineTick = 0;
	static unsigned sLastTick = ~0u;
	unsigned tick = syNetInputGetTick();
	if (tick == sLastTick) {
		return;
	}
	sLastTick = tick;
	if (tick % 300 == 0) {
		PortRollbackSave(sBaseline);
		sBaselineTick = tick;
	} else if ((tick % 300 == 1 || tick % 300 == 60) && sBaseline.size != 0) {
		static RollbackBuffer cur;
		PortRollbackSave(cur);
		size_t n = std::min(cur.size, sBaseline.size), changed = 0;
		for (size_t off = 0; off < n; off += 4096) {
			if (std::memcmp(cur.data + off, sBaseline.data + off, std::min<size_t>(4096, n - off)) != 0) {
				changed++;
			}
		}
		port_log("SSB64 RollbackProbe: ticks %u..%u snapshot=%zu KiB changed_pages=%zu\n", sBaselineTick, tick,
		         cur.size / 1024, changed);
	}
}
