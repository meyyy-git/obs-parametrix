// Pitch correction for a single voice: YIN pitch detection on a decimated copy, a scale
// snap, and a pitch-synchronous two-tap delay-line shifter.
#include "px_module.hpp"
#include "px_params.hpp"

namespace px {

class Pitch : public Module {
	enum { KEY, SCALE, SPEED, AMOUNT };
	static constexpr int kDec = 4;   // detection runs at sr / 4
	static constexpr int kWin = 256; // analysis window, decimated samples
	static constexpr int kHop = 64;  // decimated samples between estimates
	static constexpr int kRing = 8192;

public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
		const double fd = sr / kDec;
		tauMin = std::max(2, (int)(fd / 800.0));
		tauMax = (int)(fd / 70.0);
		det.assign((size_t)(kWin + tauMax + 1), 0.0f);
		diff.assign((size_t)(tauMax + 2), 0.0f);
		for (int c = 0; c < ch; c++)
			ring[c].assign(kRing, 0.0f);
		center = (int)(0.02 * sr + 0.5);
		dA = dB = center;
		period = sr / 200.0;
		aaC = OnePole::coef(sr / kDec * 0.35, sr);
		aux[0].store(-1.0f);
	}
	void setParams(const double *v) override
	{
		key = (int)clampd(v[KEY], 0, 11);
		scale = (int)clampd(v[SCALE], 0, 4);
		// retune time, applied once per estimate
		speedC = v[SPEED] < 1 ? 1.0 : 1.0 - std::exp(-(double)(kHop * kDec) / (v[SPEED] * 0.001 * sr));
		amount = v[AMOUNT] / 100.0;
	}
	int latency() const override { return center; }

	void process(float **io, int n, const Ctx &) override
	{
		for (int i = 0; i < n; i++) {
			double mono = 0;
			for (int c = 0; c < ch; c++) {
				ring[c][(size_t)wp] = io[c][i];
				mono += io[c][i];
			}
			mono /= ch;
			detect(mono);

			// The read taps move at `ratio` times the write speed, so the delay drifts.
			dA += 1.0 - ratio;
			if (fading) {
				dB += 1.0 - ratio;
				const double g = 0.5 - 0.5 * std::cos(kPi * ++fadePos / fadeLen);
				for (int c = 0; c < ch; c++)
					io[c][i] = (float)(read(c, dA) * g + read(c, dB) * (1.0 - g));
				if (fadePos >= fadeLen)
					fading = false;
			} else {
				for (int c = 0; c < ch; c++)
					io[c][i] = (float)read(c, dA);
				// When the delay drifts half a period from its centre, jump one whole period
				// and crossfade: the two taps are then in phase, so the splice is inaudible.
				const double half = period * 0.5;
				if (dA < center - half || dA > center + half) {
					dB = dA;
					dA += dA < center ? period : -period;
					fading = true;
					fadePos = 0;
					fadeLen = (int)clampd(period, 64, 900);
				}
			}
			wp = (wp + 1) & (kRing - 1);
		}
	}

private:
	// 4-point Hermite interpolation at a fractional delay behind the write position.
	inline double read(int c, double delay) const
	{
		const double pos = (double)wp - delay;
		const double fl = std::floor(pos);
		const double t = pos - fl;
		const int i = (int)fl;
		const float *b = ring[c].data();
		const double y0 = b[(i - 1) & (kRing - 1)], y1 = b[i & (kRing - 1)], y2 = b[(i + 1) & (kRing - 1)],
			     y3 = b[(i + 2) & (kRing - 1)];
		const double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3,
			     c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
		return ((c3 * t + c2) * t + c1) * t + y1;
	}

	void detect(double x)
	{
		// crude anti-alias, then keep every fourth sample
		decAcc += aa.lp(x, aaC);
		if (++decN < kDec)
			return;
		const float d = (float)(decAcc / kDec);
		decAcc = 0;
		decN = 0;
		std::memmove(det.data(), det.data() + 1, sizeof(float) * (det.size() - 1));
		det.back() = d;
		if (++hop < kHop)
			return;
		hop = 0;
		estimate();
	}

	void estimate()
	{
		// YIN: difference function, cumulative mean normalisation, first dip under the threshold
		double energy = 0;
		for (int j = 0; j < kWin; j++)
			energy += (double)det[(size_t)j] * det[(size_t)j];
		double f0 = 0;
		if (energy / kWin > 1e-6) { // above about -60 dBFS
			double run = 0;
			diff[0] = 1;
			for (int tau = 1; tau <= tauMax; tau++) {
				double s = 0;
				for (int j = 0; j < kWin; j++) {
					const double e = det[(size_t)j] - det[(size_t)(j + tau)];
					s += e * e;
				}
				run += s;
				diff[(size_t)tau] = (float)(run > 0 ? s * tau / run : 1.0);
			}
			int best = -1;
			for (int tau = tauMin; tau < tauMax; tau++)
				if (diff[(size_t)tau] < 0.15f) {
					while (tau + 1 < tauMax && diff[(size_t)(tau + 1)] < diff[(size_t)tau])
						tau++;
					best = tau;
					break;
				}
			if (best < 0) { // no clear dip: accept the deepest one only if it is convincing
				int m = tauMin;
				for (int tau = tauMin; tau < tauMax; tau++)
					if (diff[(size_t)tau] < diff[(size_t)m])
						m = tau;
				if (diff[(size_t)m] < 0.3f)
					best = m;
			}
			if (best > tauMin && best < tauMax - 1) {
				const double a = diff[(size_t)(best - 1)], b = diff[(size_t)best],
					     c = diff[(size_t)(best + 1)];
				const double den = a - 2 * b + c;
				const double tau = best + (std::fabs(den) > 1e-12 ? 0.5 * (a - c) / den : 0.0);
				f0 = sr / (tau * kDec);
			}
		}

		double target = 1.0;
		if (f0 > 0) {
			period = sr / f0;
			const double note = 69.0 + 12.0 * std::log2(f0 / 440.0);
			const double snapped = snap(note);
			target = std::pow(2.0, (snapped - note) * amount / 12.0);
			aux[0].store((float)note);
			aux[1].store((float)((snapped - note) * 100.0 * amount));
		} else {
			aux[0].store(-1.0f);
			aux[1].store(0.0f);
		}
		// glide in the log domain, so equal musical intervals take equal time
		ratio = std::exp(std::log(ratio) + (std::log(target) - std::log(ratio)) * speedC);
	}

	// Nearest note of the chosen key and scale, as a (fractional-free) MIDI number.
	double snap(double note) const
	{
		static const int scales[5][12] = {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},  // chromatic
						  {1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1},  // major
						  {1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0},  // natural minor
						  {1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0},  // major pentatonic
						  {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0}}; // minor pentatonic
		const int base = (int)std::floor(note + 0.5);
		double best = base, bestDist = 1e9;
		for (int m = base - 6; m <= base + 6; m++) {
			const int pc = (((m - key) % 12) + 12) % 12;
			if (scales[scale][pc] && std::fabs(m - note) < bestDist) {
				bestDist = std::fabs(m - note);
				best = m;
			}
		}
		return best;
	}

	double sr = 48000, period = 240, ratio = 1, speedC = 1, amount = 1, aaC = 0.5, decAcc = 0;
	double dA = 960, dB = 960;
	int ch = 2, key = 0, scale = 0, center = 960, tauMin = 15, tauMax = 171;
	int wp = 0, decN = 0, hop = 0, fadePos = 0, fadeLen = 1;
	bool fading = false;
	OnePole aa;
	std::vector<float> det, diff, ring[kMaxCh];
};

std::shared_ptr<Module> makePitch()
{
	return std::make_shared<Pitch>();
}

} // namespace px
