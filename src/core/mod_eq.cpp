// Parametric / dynamic EQ: 24 bands, Simper SVF sections, per-band stereo placement.
#include "px_module.hpp"
#include "px_params.hpp"

namespace px {

namespace {

constexpr int kMaxSec = 8;
constexpr int kTick = 16; // samples between coefficient updates

// One band's filter sections, shared by the audio path and the response curve.
struct Sections {
	int nsec = 0;
	bool onePole = false, lowCut = false;
	double opG = 0, staticGain = 1;
	SvfCoef c[kMaxSec];
};

void designSections(Sections &b, int type, int slope, double f, double q, double g, double sr)
{
	b.onePole = false;
	b.lowCut = type == EQT_LOWCUT;
	b.staticGain = 1;
	switch (type) {
	case EQT_BELL:
		b.nsec = 1;
		b.c[0] = svfCoef(Svf::Bell, f, q, g, sr);
		break;
	case EQT_LOWSHELF:
		b.nsec = 1;
		b.c[0] = svfCoef(Svf::LowShelf, f, q, g, sr);
		break;
	case EQT_HIGHSHELF:
		b.nsec = 1;
		b.c[0] = svfCoef(Svf::HighShelf, f, q, g, sr);
		break;
	case EQT_LOWCUT:
	case EQT_HIGHCUT: {
		// Butterworth cascade; the band's Q scales the resonance of the last section.
		const Svf t = type == EQT_LOWCUT ? Svf::HighPass : Svf::LowPass;
		const int order = kEqSlopes[slope] / 6;
		b.nsec = order / 2;
		b.onePole = order % 2 == 1;
		b.opG = OnePole::coef(f, sr);
		for (int k = 0; k < b.nsec; k++) {
			const double ang = b.onePole ? kPi * (k + 1) / order : kPi * (2 * k + 1) / (2.0 * order);
			double sq = 1.0 / (2.0 * std::cos(ang));
			if (k == b.nsec - 1)
				sq *= q / 0.70710678;
			b.c[k] = svfCoef(t, f, sq, 0, sr);
		}
		break;
	}
	case EQT_NOTCH:
		b.nsec = 1;
		b.c[0] = svfCoef(Svf::Notch, f, q, 0, sr);
		break;
	case EQT_BANDPASS:
		b.nsec = 1;
		b.c[0] = svfCoef(Svf::BandPass, f, q, 0, sr);
		break;
	case EQT_TILT:
		b.nsec = 2;
		b.c[0] = svfCoef(Svf::LowShelf, f, q, -g / 2, sr);
		b.c[1] = svfCoef(Svf::HighShelf, f, q, g / 2, sr);
		break;
	default: // EQT_FLATTILT
		// shortcut: five high shelves two octaves apart approximate a constant
		// dB/octave slope; replace with a true tilt filter if ripple matters.
		b.nsec = 5;
		for (int k = 0; k < 5; k++)
			b.c[k] = svfCoef(Svf::HighShelf, f * std::pow(4.0, k - 2), 0.5, g / 5, sr);
		b.staticGain = dbToLin(-g / 2);
	}
}

// The trapezoidal SVF is the bilinear transform of the analog SVF, so its response at f is the
// analog prototype's at w = tan(pi f / sr) / g:  H(s) = m0 + (m1 s + m2) / (s^2 + k s + 1).
double svfPowerGain(const SvfCoef &c, double f, double sr)
{
	const double g = c.a2 / c.a1, k = (1.0 / c.a1 - 1.0) / g - g;
	const double w = std::tan(kPi * f / sr) / g;
	const std::complex<double> s(0, w);
	return std::norm(c.m0 + (c.m1 * s + c.m2) / (s * s + k * s + 1.0));
}

// Power gain of a whole band at f.
double sectionsPower(const Sections &b, double f, double sr)
{
	double pw = b.staticGain * b.staticGain;
	if (b.onePole) {
		const double g = b.opG / (1.0 - b.opG), w = std::tan(kPi * f / sr) / g;
		pw *= b.lowCut ? w * w / (1.0 + w * w) : 1.0 / (1.0 + w * w);
	}
	for (int i = 0; i < b.nsec; i++)
		pw *= svfPowerGain(b.c[i], f, sr);
	return pw;
}

// Linear-phase path: a 4096-tap symmetric FIR built from the bands' magnitude response,
// applied by uniformly partitioned FFT convolution (16 partitions of 256 samples).
constexpr int kFirN = 4096, kPart = 256, kParts = kFirN / kPart;
constexpr int kLinearLatency = kFirN / 2 + kPart;

struct Band : Sections {
	bool on = false, solo = false, dyn = false;
	int type = EQT_BELL, place = 0, slope = 1, sc = -1;
	double th = -30, range = 0, atkC = 1, relC = 1;
	Smooth f, g, q;
	bool dirty = true;

	SvfState st[kMaxCh][kMaxSec];
	OnePole op[kMaxCh];

	SvfCoef detC;
	SvfState detSt;
	double env = 0, dynGain = 0;

	inline double run(int ch, double x)
	{
		if (onePole) {
			const double lp = op[ch].lp(x, opG);
			x = lowCut ? x - lp : lp;
		}
		for (int s = 0; s < nsec; s++)
			x = st[ch][s].tick(c[s], x);
		return x * staticGain;
	}
};

} // namespace

class Eq : public Module {
public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
		const double c16 = coefMs(20, sr, kTick);
		for (Band &b : bands)
			b.f.coef = b.g.coef = b.q.coef = c16;
		fftK.init(kFirN);
		fftP.init(2 * kPart);
		kernel.assign((size_t)kFirN, {});
		work.assign((size_t)(2 * kPart), {});
		partSpec.assign((size_t)(kParts * 2 * kPart), {});
		for (int c = 0; c < ch; c++) {
			inSpec[c].assign((size_t)(kParts * 2 * kPart), {});
			inBlock[c].assign((size_t)(2 * kPart), 0.0f);
			outBlock[c].assign((size_t)kPart, 0.0f);
		}
	}

	int latency() const override { return linear ? kLinearLatency : 0; }

	void setParams(const double *v) override
	{
		const int n = (int)clampd(v[0], 0, kEqBands);
		linear = v[1] > 0.5;
		if (linear)
			designKernel(v, n);
		for (int i = 0; i < kEqBands; i++) {
			const double *p = v + kEqFirstBand + i * kEqBandParams;
			Band &b = bands[i];
			const bool on = i < n && p[EQ_ON] > 0.5;
			const int type = (int)p[EQ_TYPE], slope = (int)clampd(p[EQ_SLOPE], 0, 7);
			if (on != b.on || type != b.type || slope != b.slope)
				b.dirty = true;
			if (on && !b.on) { // start clean so a re-enabled band doesn't replay old state
				for (auto &row : b.st)
					for (SvfState &s : row)
						s.reset();
				for (OnePole &o : b.op)
					o.s = 0;
				b.f.primed = b.g.primed = b.q.primed = false;
			}
			b.on = on;
			b.type = type;
			b.slope = slope;
			b.place = (int)p[EQ_PLACE];
			b.solo = p[EQ_SOLO] > 0.5;
			b.f.set(p[EQ_FREQ]);
			b.g.set(p[EQ_GAIN]);
			b.q.set(p[EQ_Q]);
			const bool gainType = type == EQT_BELL || type == EQT_LOWSHELF || type == EQT_HIGHSHELF ||
					      type == EQT_TILT || type == EQT_FLATTILT;
			b.dyn = p[EQ_DYN_ON] > 0.5 && gainType;
			b.th = p[EQ_DYN_TH];
			b.range = p[EQ_DYN_RANGE];
			b.atkC = coefMs(p[EQ_DYN_ATK], sr);
			b.relC = coefMs(p[EQ_DYN_REL], sr);
			b.sc = (int)p[EQ_DYN_SC];
			if (!b.dyn)
				b.dynGain = 0;
		}
	}

	void process(float **io, int n, const Ctx &ctx) override
	{
		if (linear) {
			processLinear(io, n);
			return;
		}
		int soloBand = -1;
		for (int i = 0; i < kEqBands; i++)
			if (bands[i].on && bands[i].solo) {
				soloBand = i;
				break;
			}
		const bool stereo = ch == 2;
		double minDyn = 0;

		for (int i = 0; i < n; i++) {
			if ((counter++ % kTick) == 0)
				tick();

			double x[kMaxCh], monoIn = 0;
			for (int c = 0; c < ch; c++) {
				x[c] = io[c][i];
				monoIn += x[c];
			}

			if (soloBand >= 0) {
				for (int c = 0; c < ch; c++)
					io[c][i] = (float)soloSt[c].tick(soloC, x[c]);
				// keep the band states warm so un-soloing doesn't click
			}

			for (Band &b : bands) {
				if (!b.on)
					continue;
				if (b.dyn) {
					const ScBlock *sc = sidechain(ctx, b.sc);
					double d = 0;
					if (sc) {
						d = scPeak(sc, i);
					} else {
						for (int c = 0; c < ch; c++)
							d += x[c];
						d /= ch;
					}
					d = std::fabs(b.detSt.tick(b.detC, d));
					b.env += (d - b.env) * (d > b.env ? b.atkC : b.relC);
				}
				if (stereo) {
					switch (b.place) {
					case 1:
						x[0] = b.run(0, x[0]);
						break;
					case 2:
						x[1] = b.run(1, x[1]);
						break;
					case 3: {
						const double m = b.run(0, (x[0] + x[1]) * 0.5), s = (x[0] - x[1]) * 0.5;
						x[0] = m + s;
						x[1] = m - s;
						break;
					}
					case 4: {
						const double m = (x[0] + x[1]) * 0.5, s = b.run(1, (x[0] - x[1]) * 0.5);
						x[0] = m + s;
						x[1] = m - s;
						break;
					}
					default:
						x[0] = b.run(0, x[0]);
						x[1] = b.run(1, x[1]);
					}
				} else {
					for (int c = 0; c < ch; c++)
						x[c] = b.run(c, x[c]);
				}
			}
			if (soloBand < 0)
				for (int c = 0; c < ch; c++)
					io[c][i] = (float)x[c];
			double monoOut = 0;
			for (int c = 0; c < ch; c++)
				monoOut += io[c][i];
			const unsigned w = wpos.load(std::memory_order_relaxed);
			ring[0][w & kTapMask] = (float)(monoIn / ch);
			ring[1][w & kTapMask] = (float)(monoOut / ch);
			wpos.store(w + 1, std::memory_order_relaxed);
		}
		for (const Band &b : bands)
			if (b.on && b.dyn)
				minDyn = std::min(minDyn, b.dynGain);
		gr.store((float)minDyn);
	}

	int tap(int which, float *dst, int n) override
	{
		n = std::min(n, kTapMask + 1);
		const unsigned w = wpos.load(std::memory_order_relaxed);
		for (int i = 0; i < n; i++)
			dst[i] = ring[which ? 1 : 0][(w - (unsigned)n + (unsigned)i) & kTapMask];
		return n;
	}

private:
	// Builds the FIR from the bands' static response. Runs on the audio thread when a
	// parameter changes; every buffer it touches was allocated in prepare().
	// shortcut: in linear-phase mode dynamic bands use their static gain, solo is ignored and
	// every band applies to all channels; upgrade if per-band placement is needed here.
	void designKernel(const double *v, int n)
	{
		Sections sec[kEqBands];
		int used = 0;
		for (int i = 0; i < n; i++) {
			const double *p = v + kEqFirstBand + i * kEqBandParams;
			if (p[EQ_ON] > 0.5)
				designSections(sec[used++], (int)p[EQ_TYPE], (int)clampd(p[EQ_SLOPE], 0, 7), p[EQ_FREQ],
					       p[EQ_Q], p[EQ_GAIN], sr);
		}
		for (int k = 0; k <= kFirN / 2; k++) {
			const double f = clampd(k * sr / kFirN, 1.0, sr * 0.499);
			double pw = 1;
			for (int b = 0; b < used; b++)
				pw *= sectionsPower(sec[b], f, sr);
			const float mag = (float)std::sqrt(std::min(pw, 1e6)); // zero phase: a real spectrum
			kernel[(size_t)k] = mag;
			if (k > 0 && k < kFirN / 2)
				kernel[(size_t)(kFirN - k)] = mag;
		}
		fftK.run(kernel.data(), true);
		// centre the impulse, window it, and transform each partition
		for (int j = 0; j < kParts; j++) {
			for (int i = 0; i < 2 * kPart; i++) {
				float h = 0;
				if (i < kPart) {
					const int nIdx = j * kPart + i;
					const float win = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * nIdx / kFirN));
					h = kernel[(size_t)((nIdx + kFirN / 2) % kFirN)].real() * win;
				}
				work[(size_t)i] = h;
			}
			fftP.run(work.data(), false);
			std::copy(work.begin(), work.end(), partSpec.begin() + j * 2 * kPart);
		}
	}

	void processLinear(float **io, int n)
	{
		for (int i = 0; i < n; i++) {
			double monoIn = 0, monoOut = 0;
			for (int c = 0; c < ch; c++) {
				monoIn += io[c][i];
				inBlock[c][(size_t)(kPart + lpos)] = io[c][i];
				io[c][i] = outBlock[c][(size_t)lpos];
				monoOut += io[c][i];
			}
			const unsigned w = wpos.load(std::memory_order_relaxed);
			ring[0][w & kTapMask] = (float)(monoIn / ch);
			ring[1][w & kTapMask] = (float)(monoOut / ch);
			wpos.store(w + 1, std::memory_order_relaxed);
			if (++lpos < kPart)
				continue;
			lpos = 0;
			lhead = (lhead + 1) % kParts;
			for (int c = 0; c < ch; c++) {
				// spectrum of the last two partitions of input
				std::complex<float> *x = inSpec[c].data() + lhead * 2 * kPart;
				for (int k = 0; k < 2 * kPart; k++)
					x[k] = inBlock[c][(size_t)k];
				fftP.run(x, false);
				// sum over partitions: input from j blocks ago times kernel partition j
				std::fill(work.begin(), work.end(), std::complex<float>());
				for (int j = 0; j < kParts; j++) {
					const std::complex<float> *xs =
						inSpec[c].data() + ((lhead - j + kParts) % kParts) * 2 * kPart;
					const std::complex<float> *hs = partSpec.data() + j * 2 * kPart;
					for (int k = 0; k < 2 * kPart; k++)
						work[(size_t)k] += xs[k] * hs[k];
				}
				fftP.run(work.data(), true);
				for (int k = 0; k < kPart; k++) {
					outBlock[c][(size_t)k] =
						work[(size_t)(kPart + k)].real(); // the alias-free half
					inBlock[c][(size_t)k] = inBlock[c][(size_t)(kPart + k)];
				}
			}
		}
		gr.store(0.0f);
	}

	void tick()
	{
		for (int i = 0; i < kEqBands; i++) {
			Band &b = bands[i];
			if (!b.on)
				continue;
			const bool moving = b.f.moving() || b.g.moving() || b.q.moving();
			b.f.next();
			b.g.next();
			b.q.next();
			if (b.dyn) {
				const double over = linToDb(b.env) - b.th;
				const double span = std::fabs(b.range);
				b.dynGain = span > 0.01 ? b.range * clampd(over / span, 0, 1) : 0;
			}
			if (moving || b.dyn || b.dirty)
				design(b);
			if (b.solo)
				soloC = svfCoef(Svf::BandPass, b.f.cur, std::max(b.q.cur, 1.0), 0, sr);
		}
	}

	void design(Band &b)
	{
		const double f = b.f.cur, q = b.q.cur, g = b.g.cur + b.dynGain;
		b.dirty = false;
		b.detC = svfCoef(Svf::BandPass, f, std::max(q, 1.0), 0, sr);
		designSections(b, b.type, b.slope, f, q, g, sr);
	}

	double sr = 48000;
	int ch = 2;
	unsigned counter = 0;
	Band bands[kEqBands];
	SvfCoef soloC;
	SvfState soloSt[kMaxCh];
	static constexpr int kTapMask = 4095;
	float ring[2][kTapMask + 1] = {};
	std::atomic<unsigned> wpos{0};

	bool linear = false;
	int lpos = 0, lhead = 0;
	Fft fftK, fftP;
	std::vector<std::complex<float>> kernel, work, partSpec, inSpec[kMaxCh];
	std::vector<float> inBlock[kMaxCh], outBlock[kMaxCh];
};

double eqBandDb(const double *p, double gainDb, double sr, double freq)
{
	Sections b;
	designSections(b, (int)p[EQ_TYPE], (int)clampd(p[EQ_SLOPE], 0, 7), p[EQ_FREQ], p[EQ_Q], gainDb, sr);
	return 10.0 * std::log10(std::max(sectionsPower(b, clampd(freq, 1.0, sr * 0.499), sr), 1e-30));
}

std::shared_ptr<Module> makeEq()
{
	return std::make_shared<Eq>();
}

} // namespace px
