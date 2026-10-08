#pragma once

/**
 * journaled.h — hash containers whose changes can be rewound for rollback.
 *
 * Port-side "already fixed" bookkeeping grows to thousands of entries during
 * a match, so copying it into every per-tick snapshot is too slow. These
 * wrappers log each change instead: a snapshot records the log position
 * (mark), and restoring undoes changes back to it (rewind). Cost is
 * proportional to what changed since the snapshot, not to container size.
 *
 * clear() starts a new epoch (scene changes); marks from an older epoch can't
 * be rewound to, which matches snapshots never crossing scenes.
 */

#include "rollback_io.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct JournalMark {
	uint64_t epoch;
	uint64_t position;
};

/* Non-zero if a rollback restore rewrites the memory at addr (scene arena
 * simulation region or game globals). Records about other memory — e.g.
 * files that stay loaded across scenes — describe bytes a restore does not
 * touch, so rewinding them would make the next fixup apply twice. */
extern "C" int port_rollback_addr_restored(uintptr_t addr);

/* Undoes log entries back to `position`, except entries about memory the
 * restore leaves alone; those stay applied and stay in the log. */
template <typename Entry, typename Undo>
void journal_rewind(std::vector<Entry> &log, uint64_t position, Undo undo)
{
	std::vector<Entry> kept;
	while (log.size() > position) {
		Entry e = log.back();
		log.pop_back();
		if (port_rollback_addr_restored((uintptr_t)e.key)) undo(e);
		else kept.push_back(e);
	}
	log.insert(log.end(), kept.rbegin(), kept.rend());
}

template <typename K> class JournaledSet {
public:
	using Set = std::unordered_set<K>;
	using iterator = typename Set::iterator;
	using const_iterator = typename Set::const_iterator;

	std::pair<iterator, bool> insert(const K &key)
	{
		auto result = set_.insert(key);
		if (result.second) log_.push_back({ true, key });
		return result;
	}
	size_t erase(const K &key)
	{
		size_t n = set_.erase(key);
		if (n) log_.push_back({ false, key });
		return n;
	}
	iterator erase(const_iterator it)
	{
		log_.push_back({ false, *it });
		return set_.erase(it);
	}
	void clear()
	{
		set_.clear();
		log_.clear();
		epoch_++;
	}
	size_t count(const K &key) const { return set_.count(key); }
	iterator find(const K &key) { return set_.find(key); }
	const_iterator find(const K &key) const { return set_.find(key); }
	iterator begin() { return set_.begin(); }
	iterator end() { return set_.end(); }
	const_iterator begin() const { return set_.begin(); }
	const_iterator end() const { return set_.end(); }
	size_t size() const { return set_.size(); }

	JournalMark mark() const { return { epoch_, log_.size() }; }
	bool rewind(const JournalMark &m)
	{
		if (m.epoch != epoch_ || m.position > log_.size()) return false;
		journal_rewind(log_, m.position, [&](const Entry &e) {
			if (e.inserted) set_.erase(e.key);
			else set_.insert(e.key);
		});
		return true;
	}

private:
	struct Entry {
		bool inserted;
		K key;
	};
	Set set_;
	std::vector<Entry> log_;
	uint64_t epoch_ = 0;
};

template <typename K, typename V> class JournaledMap {
public:
	using Map = std::unordered_map<K, V>;
	using iterator = typename Map::iterator;
	using const_iterator = typename Map::const_iterator;

	void set(const K &key, const V &value)
	{
		auto it = map_.find(key);
		if (it == map_.end()) {
			log_.push_back({ key, false, V{} });
			map_.emplace(key, value);
		} else {
			log_.push_back({ key, true, it->second });
			it->second = value;
		}
	}
	iterator erase(const_iterator it)
	{
		log_.push_back({ it->first, true, it->second });
		return map_.erase(it);
	}
	void clear()
	{
		map_.clear();
		log_.clear();
		epoch_++;
	}
	iterator find(const K &key) { return map_.find(key); }
	iterator begin() { return map_.begin(); }
	iterator end() { return map_.end(); }
	size_t size() const { return map_.size(); }

	JournalMark mark() const { return { epoch_, log_.size() }; }
	bool rewind(const JournalMark &m)
	{
		if (m.epoch != epoch_ || m.position > log_.size()) return false;
		journal_rewind(log_, m.position, [&](const Entry &e) {
			if (e.had_old) map_[e.key] = e.old_value;
			else map_.erase(e.key);
		});
		return true;
	}

private:
	struct Entry {
		K key;
		bool had_old;
		V old_value;
	};
	Map map_;
	std::vector<Entry> log_;
	uint64_t epoch_ = 0;
};

/* Snapshot helpers: a container contributes only its mark. */
template <typename C> void journal_save(RollbackWriter &w, const C &c)
{
	w.value(c.mark());
}
template <typename C> bool journal_load(RollbackReader &r, C &c)
{
	JournalMark m{};
	return r.value(m) && c.rewind(m);
}
