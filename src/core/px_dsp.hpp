// Parametrix DSP building blocks. Header-only, no OBS dependency.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

namespace px {

constexpr int kMaxCh = 8;
constexpr int kMaxBlock = 1024;
constexpr int kMaxSidechains = 8;
constexpr double kPi = 3.14159265358979323846;

// exp and log are markedly cheaper than pow and log10, and these run per sample
inline double dbToLin(double db)
{
	return std::exp(db * 0.11512925464970229); // ln(10) / 20
}
inline double linToDb(double lin)
{
	return 8.685889638065035 * std::log(std::max(lin, 1e-10)); // 20 / ln(10)
}
inline double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}
// One-pole coefficient that reaches ~63% of a step after `ms`.
inline double coefMs(double ms, double sr, double samplesPerTick = 1.0)
{
	return 1.0 - std::exp(-samplesPerTick / (std::max(ms, 0.01) * 0.001 * sr));
}

struct Smooth {
	double cur = 0, target = 0, coef = 1;
	bool primed = false;
	void set(double v)
	{
		target = v;
		if (!primed) {
			cur = v;
			primed = true;
		}
	}
	double next()
	{
		cur += (target - cur) * coef;
		return cur;
	}
	bool moving() const { return std::fabs(target - cur) > 1e-6 * (1.0 + std::fabs(target)); }
};

// Andrew Simper (Cytomic) trapezoidal SVF, mix-output form.
enum class Svf { Bell, LowShelf, HighShelf, LowPass, HighPass, Notch, BandPass };

struct SvfCoef {
	double a1 = 1, a2 = 0, a3 = 0, m0 = 1, m1 = 0, m2 = 0;
};

inline SvfCoef svfCoef(Svf type, double f, double q, double gainDb, double sr)
{
	f = clampd(f, 10.0, sr * 0.49);
	q = std::max(q, 0.025);
	const double A = std::pow(10.0, gainDb / 40.0);
	double g = std::tan(kPi * f / sr);
	double k = 1.0 / q;
	SvfCoef c;
	switch (type) {
	case Svf::Bell:
		k = 1.0 / (q * A);
		c.m0 = 1;
		c.m1 = k * (A * A - 1);
		c.m2 = 0;
		break;
	case Svf::LowShelf:
		g /= std::sqrt(A);
		c.m0 = 1;
		c.m1 = k * (A - 1);
		c.m2 = A * A - 1;
		break;
	case Svf::HighShelf:
		g *= std::sqrt(A);
		c.m0 = A * A;
		c.m1 = k * (1 - A) * A;
		c.m2 = 1 - A * A;
		break;
	case Svf::LowPass:
		c.m0 = 0;
		c.m1 = 0;
		c.m2 = 1;
		break;
	case Svf::HighPass:
		c.m0 = 1;
		c.m1 = -k;
		c.m2 = -1;
		break;
	case Svf::Notch:
		c.m0 = 1;
		c.m1 = -k;
		c.m2 = 0;
		break;
	case Svf::BandPass: // 0 dB at the centre frequency
		c.m0 = 0;
		c.m1 = k;
		c.m2 = 0;
		break;
	}
	c.a1 = 1.0 / (1.0 + g * (g + k));
	c.a2 = g * c.a1;
	c.a3 = g * c.a2;
	return c;
}

struct SvfState {
	double ic1 = 0, ic2 = 0;
	inline double tick(const SvfCoef &c, double v0)
	{
		const double v3 = v0 - ic2;
		const double v1 = c.a1 * ic1 + c.a2 * v3;
		const double v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
		ic1 = 2 * v1 - ic1;
		ic2 = 2 * v2 - ic2;
		return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
	}
	void reset() { ic1 = ic2 = 0; }
};

// First-order TPT low-pass; high-pass is x - lp.
struct OnePole {
	double s = 0;
	inline double lp(double x, double G)
	{
		const double v = (x - s) * G;
		const double y = v + s;
		s = y + v;
		return y;
	}
	static double coef(double f, double sr)
	{
		const double g = std::tan(kPi * clampd(f, 1.0, sr * 0.49) / sr);
		return g / (1.0 + g);
	}
};

struct Biquad {
	double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
	inline double tick(double x)
	{
		const double y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}
};

// Linkwitz-Riley 4th-order split. lo + hi is an all-pass of the input.
struct LR4 {
	SvfCoef lpc, hpc;
	SvfState l1, l2, h1, h2;
	void set(double f, double sr)
	{
		lpc = svfCoef(Svf::LowPass, f, 0.70710678, 0, sr);
		hpc = svfCoef(Svf::HighPass, f, 0.70710678, 0, sr);
	}
	inline void split(double x, double &lo, double &hi)
	{
		lo = l2.tick(lpc, l1.tick(lpc, x));
		hi = h2.tick(hpc, h1.tick(hpc, x));
	}
	inline double allpass(double x)
	{
		double lo, hi;
		split(x, lo, hi);
		return lo + hi;
	}
};

class DelayLine {
public:
	void resize(int maxSamples)
	{
		int n = 1;
		while (n < maxSamples + 2)
			n <<= 1;
		buf.assign((size_t)n, 0.0f);
		mask = n - 1;
		w = 0;
	}
	inline void write(float x)
	{
		buf[(size_t)w] = x;
		w = (w + 1) & mask;
	}
	// delay in samples, 1 = the sample written by the previous write()
	inline float read(int delay) const { return buf[(size_t)((w - delay) & mask)]; }
	inline float readFrac(double delay) const
	{
		const int d = (int)delay;
		const float fr = (float)(delay - d);
		const float a = read(d), b = read(d + 1);
		return a + (b - a) * fr;
	}
	int capacity() const { return mask; }

private:
	std::vector<float> buf;
	int mask = 0, w = 0;
};

// 2x oversampling with a 31-tap windowed-sinc half-band filter.
// Up then down adds 15 samples of latency at the base rate.
class Halfband2x {
public:
	static constexpr int N = 31;
	static constexpr int kLatency = 15;
	Halfband2x()
	{
		static const Taps t;
		taps = t.h;
	}
	inline void up(float x, float &y0, float &y1)
	{
		uh[upos] = x;
		// Zero-stuffed convolution split into its two polyphase branches. A half-band filter's
		// odd taps are zero except the centre one, so the second branch is a single product.
		double a = 0;
		for (int j = 0; j < 16; j++)
			a += taps[2 * j] * uh[(upos - j) & 15];
		const double b = taps[N / 2] * uh[(upos - N / 4) & 15];
		upos = (upos + 1) & 15;
		y0 = (float)(2.0 * a);
		y1 = (float)(2.0 * b);
	}
	inline float down(float x0, float x1)
	{
		dh[dpos] = x0;
		dpos = (dpos + 1) & 31;
		dh[dpos] = x1;
		dpos = (dpos + 1) & 31;
		double a = taps[N / 2] * dh[(dpos - 1 - N / 2) & 31];
		for (int i = 0; i < N; i += 2) // the other odd taps are zero
			a += taps[i] * dh[(dpos - 1 - i) & 31];
		return (float)a;
	}

private:
	struct Taps {
		double h[N];
		Taps()
		{
			double sum = 0;
			for (int i = 0; i < N; i++) {
				const double m = i - (N - 1) / 2.0;
				const double sinc = m == 0 ? 0.5 : std::sin(kPi * 0.5 * m) / (kPi * m);
				const double win = 0.42 - 0.5 * std::cos(2 * kPi * i / (N - 1)) +
						   0.08 * std::cos(4 * kPi * i / (N - 1));
				h[i] = sinc * win;
				sum += h[i];
			}
			for (double &v : h)
				v /= sum;
		}
	};
	const double *taps;
	float uh[16] = {}, dh[32] = {};
	int upos = 0, dpos = 0;
};

// In-place radix-2 FFT.
class Fft {
public:
	void init(int n)
	{
		size = n;
		rev.resize((size_t)n);
		tw.resize((size_t)n / 2);
		int bits = 0;
		while ((1 << bits) < n)
			bits++;
		for (int i = 0; i < n; i++) {
			int r = 0;
			for (int b = 0; b < bits; b++)
				if (i & (1 << b))
					r |= 1 << (bits - 1 - b);
			rev[(size_t)i] = r;
		}
		for (int i = 0; i < n / 2; i++)
			tw[(size_t)i] = std::polar(1.0f, (float)(-2.0 * kPi * i / n));
	}
	void run(std::complex<float> *x, bool inverse) const
	{
		const int n = size;
		for (int i = 0; i < n; i++)
			if (rev[(size_t)i] > i)
				std::swap(x[i], x[rev[(size_t)i]]);
		for (int len = 2; len <= n; len <<= 1) {
			const int half = len / 2, step = n / len;
			for (int i = 0; i < n; i += len)
				for (int j = 0; j < half; j++) {
					std::complex<float> w = tw[(size_t)(j * step)];
					if (inverse)
						w = std::conj(w);
					const std::complex<float> u = x[i + j], v = x[i + j + half] * w;
					x[i + j] = u + v;
					x[i + j + half] = u - v;
				}
		}
		if (inverse) {
			const float s = 1.0f / n;
			for (int i = 0; i < n; i++)
				x[i] *= s;
		}
	}
	int size = 0;

private:
	std::vector<int> rev;
	std::vector<std::complex<float>> tw;
};

// Gain change in dB (<= 0) of a downward compressor with a soft knee.
inline double compGainDb(double levelDb, double threshDb, double ratio, double kneeDb)
{
	const double d = levelDb - threshDb;
	const double s = 1.0 / std::max(ratio, 1.0) - 1.0;
	if (kneeDb > 0.01) {
		if (2 * d < -kneeDb)
			return 0;
		if (2 * std::fabs(d) <= kneeDb)
			return s * (d + kneeDb / 2) * (d + kneeDb / 2) / (2 * kneeDb);
	} else if (d <= 0) {
		return 0;
	}
	return s * d;
}

} // namespace px
