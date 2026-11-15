// Gate/expander, leveler, de-esser, compressor, multiband compressor, limiter.
#include "px_module.hpp"
#include "px_params.hpp"

namespace px {

// ---------------------------------------------------------------- Gate / expander
class Gate : public Module {
	enum { TH, RANGE, RATIO, ATK, HOLD, REL, HYS, HPF, LPF, SC };

public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
		envRel = coefMs(5, sr);
	}
	void setParams(const double *v) override
	{
		th = v[TH];
		range = v[RANGE];
		ratio = v[RATIO];
		atkC = coefMs(v[ATK], sr);
		relC = coefMs(v[REL], sr);
		hold = (int)(v[HOLD] * 0.001 * sr);
		hys = v[HYS];
		hp = svfCoef(Svf::HighPass, v[HPF], 0.7071, 0, sr);
		lp = svfCoef(Svf::LowPass, v[LPF], 0.7071, 0, sr);
		useHp = v[HPF] > 21;
		useLp = v[LPF] < 19900;
		sc = (int)v[SC];
	}
	void process(float **io, int n, const Ctx &ctx) override
	{
		const ScBlock *s = sidechain(ctx, sc);
		double minGr = 0;
		for (int i = 0; i < n; i++) {
			double d = 0;
			if (s) {
				d = scPeak(s, i);
			} else {
				for (int c = 0; c < ch; c++) {
					double x = io[c][i];
					if (useHp)
						x = hpSt[c].tick(hp, x);
					if (useLp)
						x = lpSt[c].tick(lp, x);
					d = std::max(d, std::fabs(x));
				}
			}
			env = d > env ? d : env + (d - env) * envRel;
			const double lvl = linToDb(env);
			if (open) {
				if (lvl < th - hys) {
					if (holdLeft > 0)
						holdLeft--;
					else
						open = false;
				} else {
					holdLeft = hold;
				}
			} else if (lvl > th) {
				open = true;
				holdLeft = hold;
			}
			const double target = open ? 0.0 : std::max(range, (lvl - th) * (ratio - 1.0));
			cur += (target - cur) * (target > cur ? atkC : relC);
			minGr = std::min(minGr, cur);
			const float g = (float)dbToLin(cur);
			for (int c = 0; c < ch; c++)
				io[c][i] *= g;
		}
		gr.store((float)minGr);
	}

private:
	double sr = 48000, th = -52, range = -30, ratio = 4, atkC = 1, relC = 1, hys = 4, envRel = 1;
	double env = 0, cur = 0;
	int ch = 2, hold = 0, holdLeft = 0, sc = -1;
	bool open = false, useHp = false, useLp = false;
	SvfCoef hp, lp;
	SvfState hpSt[kMaxCh], lpSt[kMaxCh];
};

// ---------------------------------------------------------------- Leveler
class Leveler : public Module {
	enum { TARGET, MAXCH, SPEED };

public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
		rmsC = coefMs(400, sr);
	}
	void setParams(const double *v) override
	{
		target = v[TARGET];
		maxCh = v[MAXCH];
		gainC = coefMs(300 + (1.0 - v[SPEED] / 100.0) * 4700, sr);
	}
	void process(float **io, int n, const Ctx &) override
	{
		for (int i = 0; i < n; i++) {
			double p = 0;
			for (int c = 0; c < ch; c++)
				p += (double)io[c][i] * io[c][i];
			ms += (p / ch - ms) * rmsC;
			const double lvl = 10.0 * std::log10(std::max(ms, 1e-12));
			// Hold the gain while the input is near the noise floor, so pauses aren't pumped up.
			if (lvl > -50)
				cur += (clampd(target - lvl, -maxCh, maxCh) - cur) * gainC;
			const float g = (float)dbToLin(cur);
			for (int c = 0; c < ch; c++)
				io[c][i] *= g;
		}
		gr.store((float)cur);
	}

private:
	double sr = 48000, target = -18, maxCh = 12, rmsC = 1, gainC = 1, ms = 0, cur = 0;
	int ch = 2;
};

// ---------------------------------------------------------------- De-esser
class DeEsser : public Module {
	enum { FREQ, TH, RANGE, MODE, LISTEN };

public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
		atkC = coefMs(1, sr);
		relC = coefMs(50, sr);
		envRel = coefMs(3, sr);
	}
	void setParams(const double *v) override
	{
		freq = v[FREQ];
		hp = svfCoef(Svf::HighPass, freq, 0.7071, 0, sr);
		th = v[TH];
		range = v[RANGE];
		wide = v[MODE] > 0.5;
		listen = v[LISTEN] > 0.5;
	}
	void process(float **io, int n, const Ctx &) override
	{
		double minGr = 0;
		for (int i = 0; i < n; i++) {
			double d = 0;
			for (int c = 0; c < ch; c++)
				d = std::max(d, std::fabs(st[c].tick(hp, io[c][i])));
			env = d > env ? d : env + (d - env) * envRel;
			const double target = -clampd(linToDb(env) - th, 0, -range);
			cur += (target - cur) * (target < cur ? atkC : relC);
			minGr = std::min(minGr, cur);
			// Split band turns the highs down with a moving high shelf, which stays phase-coherent
			// with the lows; subtracting a filtered band would not.
			if (!wide && (counter++ & 15) == 0)
				shelf = svfCoef(Svf::HighShelf, freq, 0.7071, cur, sr);
			const double g = dbToLin(cur);
			for (int c = 0; c < ch; c++) {
				const double x = io[c][i];
				const double y = wide ? x * g : shelfSt[c].tick(shelf, x);
				io[c][i] = (float)(listen ? x - y : y);
			}
		}
		gr.store((float)minGr);
	}

private:
	double sr = 48000, th = -28, range = -8, atkC = 1, relC = 1, envRel = 1, env = 0, cur = 0;
	int ch = 2;
	double freq = 6500;
	unsigned counter = 0;
	bool wide = false, listen = false;
	SvfCoef hp, shelf;
	SvfState st[kMaxCh], shelfSt[kMaxCh];
};

// ---------------------------------------------------------------- Compressor
class Compressor : public Module {
	enum { TH, RATIO, KNEE, ATK, REL, AUTOREL, MAKEUP, AUTOMK, MIX, DET, HPF, CHAR, SC };

public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
		rmsC = coefMs(10, sr);
		slowC = coefMs(1000, sr);
		mk.coef = coefMs(20, sr);
		mix.coef = coefMs(20, sr);
	}
	void setParams(const double *v) override
	{
		th = v[TH];
		ratio = v[RATIO];
		knee = v[KNEE];
		atkC = coefMs(v[ATK], sr);
		relMs = v[REL];
		autoRel = v[AUTOREL] > 0.5;
		// Auto makeup gives back half of the reduction a full-scale signal would get.
		mk.set(v[AUTOMK] > 0.5 ? -compGainDb(0, th, ratio, knee) * 0.5 : v[MAKEUP]);
		mix.set(v[MIX] / 100.0);
		rms = v[DET] > 0.5;
		hp = svfCoef(Svf::HighPass, v[HPF], 0.7071, 0, sr);
		useHp = v[HPF] > 21;
		opto = v[CHAR] > 0.5;
		sc = (int)v[SC];
	}
	void process(float **io, int n, const Ctx &ctx) override
	{
		const ScBlock *s = sidechain(ctx, sc);
		double minGr = 0;
		for (int i = 0; i < n; i++) {
			double d = 0;
			if (s) {
				d = scPeak(s, i);
			} else {
				for (int c = 0; c < ch; c++) {
					double x = io[c][i];
					if (useHp)
						x = hpSt[c].tick(hp, x);
					d = std::max(d, std::fabs(x)); // stereo-linked detection
				}
			}
			double lvl;
			if (rms) {
				ms += (d * d - ms) * rmsC;
				lvl = 10.0 * std::log10(std::max(ms, 1e-12));
			} else {
				lvl = linToDb(d);
			}
			const double target = compGainDb(lvl, th, ratio, knee);
			if (target < cur) {
				cur += (target - cur) * atkC;
			} else {
				double rel = relMs;
				if (opto) // fast at first, slower as the reduction lets go
					rel *= cur < -6 ? 0.5 : 0.5 + 1.5 * (1.0 + cur / 6.0);
				if (autoRel) // transients well below the average reduction recover quickly
					rel *= cur < slow - 3 ? 0.25 : 1.0;
				cur += (target - cur) * coefMs(rel, sr);
			}
			slow += (cur - slow) * slowC;
			minGr = std::min(minGr, cur);
			const double m = mix.next();
			const double g = 1.0 - m + m * dbToLin(cur + mk.next());
			for (int c = 0; c < ch; c++)
				io[c][i] = (float)(io[c][i] * g);
		}
		gr.store((float)minGr);
	}

private:
	double sr = 48000, th = -20, ratio = 3, knee = 6, atkC = 1, relMs = 120, rmsC = 1, slowC = 1;
	double ms = 0, cur = 0, slow = 0;
	Smooth mk, mix;
	int ch = 2, sc = -1;
	bool autoRel = false, rms = true, useHp = false, opto = false;
	SvfCoef hp;
	SvfState hpSt[kMaxCh];
};

// ---------------------------------------------------------------- Multiband compressor
class Multiband : public Module {
	enum { X1, X2, X3 };
	enum { TH, RATIO, ATK, REL, MAKEUP, SOLO };

public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
	}
	void setParams(const double *v) override
	{
		const double x1 = v[X1], x2 = std::max(v[X2], x1 * 1.26), x3 = std::max(v[X3], x2 * 1.26);
		for (int c = 0; c < kMaxCh; c++) {
			s1[c].set(x1, sr);
			ap1[c].set(x1, sr);
			s2[c].set(x2, sr);
			s3[c].set(x3, sr);
			ap3[c].set(x3, sr);
		}
		anySolo = false;
		for (int b = 0; b < kMbBands; b++) {
			const double *p = v + kMbFirstBand + b * kMbBandParams;
			th[b] = p[TH];
			ratio[b] = p[RATIO];
			atkC[b] = coefMs(p[ATK], sr);
			relC[b] = coefMs(p[REL], sr);
			makeup[b] = p[MAKEUP];
			solo[b] = p[SOLO] > 0.5;
			anySolo = anySolo || solo[b];
		}
		sc = (int)v[kMbFirstBand + kMbBands * kMbBandParams];
	}
	void process(float **io, int n, const Ctx &ctx) override
	{
		const ScBlock *side = sidechain(ctx, sc);
		double minGr[kMbBands] = {};
		for (int i = 0; i < n; i++) {
			double band[kMaxCh][kMbBands], peak[kMbBands] = {};
			for (int c = 0; c < ch; c++) {
				double lo, hi;
				s2[c].split(io[c][i], lo, hi);
				// Each half passes the other half's crossover as an all-pass, so the
				// four bands sum back to a flat response.
				lo = ap3[c].allpass(lo);
				hi = ap1[c].allpass(hi);
				s1[c].split(lo, band[c][0], band[c][1]);
				s3[c].split(hi, band[c][2], band[c][3]);
				for (int b = 0; b < kMbBands; b++)
					peak[b] = std::max(peak[b], std::fabs(band[c][b]));
			}
			if (side) { // an external sidechain drives every band with its overall level
				const double d = scPeak(side, i);
				for (double &pk : peak)
					pk = d;
			}
			double g[kMbBands];
			for (int b = 0; b < kMbBands; b++) {
				const double target = compGainDb(linToDb(peak[b]), th[b], ratio[b], 6);
				cur[b] += (target - cur[b]) * (target < cur[b] ? atkC[b] : relC[b]);
				minGr[b] = std::min(minGr[b], cur[b]);
				g[b] = anySolo && !solo[b] ? 0.0 : dbToLin(cur[b] + makeup[b]);
			}
			for (int c = 0; c < ch; c++) {
				double y = 0;
				for (int b = 0; b < kMbBands; b++)
					y += band[c][b] * g[b];
				io[c][i] = (float)y;
			}
		}
		double worst = 0;
		for (int b = 0; b < kMbBands; b++) {
			aux[b].store((float)minGr[b]);
			worst = std::min(worst, minGr[b]);
		}
		gr.store((float)worst);
	}

private:
	double sr = 48000;
	int ch = 2;
	LR4 s1[kMaxCh], s2[kMaxCh], s3[kMaxCh], ap1[kMaxCh], ap3[kMaxCh];
	double th[kMbBands] = {}, ratio[kMbBands] = {1, 1, 1, 1}, atkC[kMbBands] = {}, relC[kMbBands] = {};
	double makeup[kMbBands] = {}, cur[kMbBands] = {};
	bool solo[kMbBands] = {}, anySolo = false;
	int sc = -1;
};

// ---------------------------------------------------------------- Limiter
// Look-ahead brickwall: sliding minimum of the required gain, release, then a moving
// average over the look-ahead, applied to the delayed signal. The average of gains that
// are each <= the required gain can never exceed it, so the ceiling always holds.
class Limiter : public Module {
	enum { CEIL, REL, TP };
	// The 4x detector's two half-band stages delay its reading by 11.25 samples.
	static constexpr int kTpDelay = 12;

public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
		L = std::max(1, (int)(0.0015 * sr + 0.5));
		for (int c = 0; c < ch; c++)
			dl[c].resize(L + 1 + kTpDelay);
		int cap = 1;
		while (cap < L + 2)
			cap <<= 1;
		qv.assign((size_t)cap, 1.0);
		qi.assign((size_t)cap, 0);
		qmask = cap - 1;
		avg.assign((size_t)L, 1.0);
		sum = L;
	}
	void setParams(const double *v) override
	{
		ceil = dbToLin(v[CEIL]);
		relC = coefMs(v[REL], sr);
		truePeak = v[TP] > 0.5;
	}
	int latency() const override { return L + (truePeak ? kTpDelay : 0); }
	void process(float **io, int n, const Ctx &) override
	{
		double minG = 1;
		for (int i = 0; i < n; i++) {
			double peak = 0;
			if (truePeak) {
				// Peaks between samples: read the level from a 4x oversampled copy.
				for (int c = 0; c < ch; c++) {
					float a, b, q[4];
					up1[c].up(io[c][i], a, b);
					up2[c].up(a, q[0], q[1]);
					up2[c].up(b, q[2], q[3]);
					for (float v : q)
						peak = std::max(peak, (double)std::fabs(v));
				}
			} else {
				for (int c = 0; c < ch; c++)
					peak = std::max(peak, (double)std::fabs(io[c][i]));
			}
			const double need = peak > ceil ? ceil / peak : 1.0;

			// monotonic queue: minimum of `need` over the last L+1 samples
			while (qhead != qtail && qv[(size_t)((qtail - 1) & qmask)] >= need)
				qtail = (qtail - 1) & qmask;
			qv[(size_t)qtail] = need;
			qi[(size_t)qtail] = t;
			qtail = (qtail + 1) & qmask;
			if (t - qi[(size_t)qhead] > (unsigned)L)
				qhead = (qhead + 1) & qmask;
			const double gmin = qv[(size_t)qhead];

			rel = gmin < rel ? gmin : rel + (gmin - rel) * relC;

			sum += rel - avg[(size_t)apos];
			avg[(size_t)apos] = rel;
			apos = (apos + 1) % L;
			const double g = std::min(sum / L, 1.0);
			minG = std::min(minG, g);

			for (int c = 0; c < ch; c++) {
				dl[c].write(io[c][i]);
				// the clamp only guards against rounding in the running sum
				io[c][i] =
					(float)clampd(dl[c].read(L + 1 + (truePeak ? kTpDelay : 0)) * g, -ceil, ceil);
			}
			t++;
		}
		// keep the running sum from drifting
		if ((t & 0xFFFF) < (unsigned)n) {
			sum = 0;
			for (double a : avg)
				sum += a;
		}
		gr.store((float)linToDb(minG));
	}

private:
	double sr = 48000, ceil = 0.89, relC = 1, rel = 1, sum = 1;
	int ch = 2, L = 72, qmask = 0, qhead = 0, qtail = 0, apos = 0;
	unsigned t = 0;
	bool truePeak = false;
	DelayLine dl[kMaxCh];
	Halfband2x up1[kMaxCh], up2[kMaxCh];
	std::vector<double> qv, avg;
	std::vector<unsigned> qi;
};

std::shared_ptr<Module> makeGate()
{
	return std::make_shared<Gate>();
}
std::shared_ptr<Module> makeLeveler()
{
	return std::make_shared<Leveler>();
}
std::shared_ptr<Module> makeDeEsser()
{
	return std::make_shared<DeEsser>();
}
std::shared_ptr<Module> makeCompressor()
{
	return std::make_shared<Compressor>();
}
std::shared_ptr<Module> makeMultiband()
{
	return std::make_shared<Multiband>();
}
std::shared_ptr<Module> makeLimiter()
{
	return std::make_shared<Limiter>();
}

} // namespace px
