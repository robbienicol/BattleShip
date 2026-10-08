#pragma once

/* Byte-stream helpers for rollback snapshots, and the save/load hooks of
 * port modules whose state is part of the simulation. C++ only. */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

/* Growable byte buffer that only ever memcpy's. (std::vector appends copy
 * element by element when the build disables inlining, as BattleShip's
 * release flags do, which made a 1.4 MB snapshot take milliseconds.) */
struct RollbackBuffer {
	uint8_t *data = nullptr;
	size_t size = 0;
	size_t cap = 0;

	RollbackBuffer() = default;
	RollbackBuffer(const RollbackBuffer &) = delete;
	RollbackBuffer &operator=(const RollbackBuffer &) = delete;
	RollbackBuffer(RollbackBuffer &&o) noexcept : data(o.data), size(o.size), cap(o.cap)
	{
		o.data = nullptr;
		o.size = o.cap = 0;
	}
	RollbackBuffer &operator=(RollbackBuffer &&o) noexcept
	{
		std::swap(data, o.data);
		std::swap(size, o.size);
		std::swap(cap, o.cap);
		return *this;
	}
	~RollbackBuffer() { std::free(data); }

	void clear() { size = 0; }
	/* Appends n uninitialized bytes and returns where they start. */
	uint8_t *grow(size_t n)
	{
		if (size + n > cap) {
			size_t next = std::max(cap * 2, size + n);
			data = static_cast<uint8_t *>(std::realloc(data, next));
			cap = next;
		}
		uint8_t *at = data + size;
		size += n;
		return at;
	}
	void assign(const RollbackBuffer &other)
	{
		clear();
		std::memcpy(grow(other.size), other.data, other.size);
	}
};

struct RollbackWriter {
	RollbackBuffer &out;

	void bytes(const void *src, size_t len)
	{
		if (len != 0) std::memcpy(out.grow(len), src, len);
	}
	template <typename T> void value(const T &v) { bytes(&v, sizeof(T)); }
	template <typename T> void array(const std::vector<T> &v)
	{
		value<uint64_t>(v.size());
		bytes(v.data(), v.size() * sizeof(T));
	}
};

struct RollbackReader {
	const uint8_t *at;
	const uint8_t *end;
	bool ok = true;

	bool bytes(void *dst, size_t len)
	{
		if (!ok || (size_t)(end - at) < len) {
			return ok = false;
		}
		std::memcpy(dst, at, len);
		at += len;
		return true;
	}
	template <typename T> bool value(T &v) { return bytes(&v, sizeof(T)); }
	template <typename T> bool array(std::vector<T> &v)
	{
		uint64_t n = 0;
		if (!value(n) || (size_t)(end - at) < n * sizeof(T)) {
			return ok = false;
		}
		v.resize((size_t)n);
		return bytes(v.data(), (size_t)n * sizeof(T));
	}
};

/* port/port_aobj_fixup.cpp: figatree un-halfswap bookkeeping, keyed by the
 * addresses of animation data that gameplay reads. */
void port_aobj_fixup_save(RollbackWriter &w);
bool port_aobj_fixup_load(RollbackReader &r);

/* port/bridge/lbreloc_bridge.cpp: file-loader bookkeeping (loaded-file list
 * counts, extern heap cursor, registered file ranges). The file lists
 * themselves are decomp globals and are restored with them. */
void port_lbreloc_save(RollbackWriter &w);
bool port_lbreloc_load(RollbackReader &r);

/* port/bridge/lbreloc_byteswap.cpp: "already byte-swapped" bookkeeping for
 * in-place fixups of file data (sprites, bitmaps, textures, vertices). */
void port_byteswap_fixups_save(RollbackWriter &w);
bool port_byteswap_fixups_load(RollbackReader &r);
