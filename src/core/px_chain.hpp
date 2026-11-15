// The rack: an ordered list of modules processed in place.
//
// Threading: configure() runs on a UI thread and may allocate. process() runs on the
// audio thread and neither locks (it only try-locks to pick up a new configuration)
// nor allocates.
#pragma once

#include "px_module.hpp"
#include "px_settings.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace px {

class Chain {
public:
	Chain();

	void configure(const ChainCfg &cfg, double sampleRate, int channels);
	void process(float **io, int channels, int frames, const ScBlock *sc = nullptr, int nsc = 0);

	int latencySamples() const { return latency.load(); }
	// UI-side lookup for meters; nullptr if the id is not in the rack.
	std::shared_ptr<Module> module(const std::string &id) const;

private:
	struct Entry {
		std::shared_ptr<Module> mod;
		bool on = true;
		std::vector<double> values;
	};
	struct Snapshot {
		double sr = 48000;
		int ch = 2;
		std::vector<Entry> entries;
	};
	struct Slot {
		std::shared_ptr<Module> mod;
		bool on = true, dying = false, muted = false;
		float gain = 0;
		double tail = 0;
		const Module *anchor = nullptr;
	};

	void adopt();
	void block(float **io, int n, const ScBlock *sc, int nsc);
	void bury(std::shared_ptr<Module> &m);

	// UI side
	mutable std::mutex uiMutex;
	std::map<std::string, std::pair<std::string, std::shared_ptr<Module>>> instances;
	double uiSr = 0;
	int uiCh = 0;

	// handoff
	std::mutex swapMutex;
	std::shared_ptr<Snapshot> pending;
	std::vector<std::shared_ptr<void>> trash;

	// audio side
	std::shared_ptr<Snapshot> cur;
	std::vector<Slot> live, next;
	std::vector<std::shared_ptr<Module>> grave;
	bool first = true;
	std::atomic<int> latency{0};
	std::vector<float> scratch[kMaxCh], dryBuf[kMaxCh];
	DelayLine dryLine[kMaxCh];
	float *scratchPtr[kMaxCh] = {}, *dryPtr[kMaxCh] = {};
};

} // namespace px
