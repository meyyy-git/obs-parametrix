// Module interface. One instance per module in the rack.
#pragma once

#include "px_dsp.hpp"

#include <memory>
#include <string>

namespace px {

// One sidechain source for the current block (planar float, same length as the block).
struct ScBlock {
	const float *ch[kMaxCh] = {};
	int nch = 0;
};

struct Ctx {
	const ScBlock *sc = nullptr; // indexed by sidechain slot
	int nsc = 0;
	float **dry = nullptr; // chain input, delayed by the chain latency
	int ch = 0;
};

class Module {
public:
	Module()
	{
		for (auto &a : aux)
			a.store(0.0f);
	}
	virtual ~Module() = default;

	// UI thread. May allocate.
	virtual void prepare(double sr, int ch) = 0;
	// Audio thread. `v` follows the module's ParamDef order. Must not allocate.
	virtual void setParams(const double *v) = 0;
	// Audio thread, in place, n <= kMaxBlock.
	virtual void process(float **io, int n, const Ctx &ctx) = 0;

	virtual int latency() const { return 0; }
	// Reverb and delay keep running this long after bypass so tails ring out.
	virtual double tailSeconds() const { return 0; }
	virtual void setInputMuted(bool) {}
	// Optional signal tap for an analyzer (UI thread): copies the newest n mono samples of
	// tap `which` (0 = module input, 1 = module output) and returns how many it wrote.
	virtual int tap(int /*which*/, float * /*dst*/, int /*n*/) { return 0; }

	// Meters, written by the audio thread, read by the UI.
	std::atomic<float> gr{0.0f}; // gain change in dB (negative = reduction)
	std::atomic<float> aux[4];

protected:
	// Mono sum of a sidechain slot, or nullptr if that slot has no audio this block.
	static const ScBlock *sidechain(const Ctx &ctx, int slot)
	{
		if (slot < 0 || slot >= ctx.nsc || !ctx.sc || ctx.sc[slot].nch <= 0)
			return nullptr;
		return &ctx.sc[slot];
	}
	static inline double scPeak(const ScBlock *sc, int i)
	{
		double p = 0;
		for (int c = 0; c < sc->nch; c++)
			p = std::max(p, (double)std::fabs(sc->ch[c][i]));
		return p;
	}
};

std::shared_ptr<Module> createModule(const std::string &type);

} // namespace px
