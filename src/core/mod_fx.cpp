// Trim, saturation, reverb, delay, output (with LUFS metering).
#include "px_module.hpp"
#include "px_params.hpp"

namespace px {

// Dry/wet law that keeps the dry signal at full level up to 50%.
static inline void mixLaw(double m, double &dry, double &wet)
{
	dry = std::min(1.0, 2.0 * (1.0 - m));
	wet = std::min(1.0, 2.0 * m);
}

// ---------------------------------------------------------------- Trim
class Trim : public Module {
	enum { GAIN, INVERT, ALIGN };

public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
		for (int c = 0; c < ch; c++)
			dl[c].resize((int)(0.5 * sr) + 2);
		g.coef = coefMs(20, sr);
	}
	void setParams(const double *v) override
	{
		g.set(dbToLin(v[GAIN]) * (v[INVERT] > 0.5 ? -1.0 : 1.0));
		// shortcut: the delay time jumps, so changing it while audio plays can click;
		// crossfade between two taps if that turns out to matter.
		delay = (int)(v[ALIGN] * 0.001 * sr + 0.5);
	}
	void process(float **io, int n, const Ctx &) override
	{
		float peak = 0;
		for (int i = 0; i < n; i++) {
			const float gg = (float)g.next();
			for (int c = 0; c < ch; c++) {
				dl[c].write(io[c][i]);
				const float y = dl[c].read(delay + 1) * gg;
				io[c][i] = y;
				peak = std::max(peak, std::fabs(y));
			}
		}
		aux[0].store((float)linToDb(peak));
	}

private:
	double sr = 48000;
	int ch = 2, delay = 0;
	Smooth g;
	DelayLine dl[kMaxCh];
};

// ---------------------------------------------------------------- Saturation
class Saturation : public Module {
	enum { DRIVE, TYPE, TONE, MIX };

public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
		for (int c = 0; c < ch; c++)
			dry[c].resize(Halfband2x::kLatency + 1);
		drive.coef = mix.coef = coefMs(20, sr);
		dcG = OnePole::coef(10, sr);
	}
	void setParams(const double *v) override
	{
		drive.set(dbToLin(v[DRIVE]));
		type = (int)v[TYPE];
		tone = svfCoef(Svf::HighShelf, 2000, 0.7071, v[TONE] * 0.06, sr);
		mix.set(v[MIX] / 100.0);
	}
	int latency() const override { return Halfband2x::kLatency; }
	void process(float **io, int n, const Ctx &) override
	{
		for (int i = 0; i < n; i++) {
			const double d = drive.next(), m = mix.next();
			const double comp = 1.0 / std::sqrt(d); // keep loudness roughly constant
			for (int c = 0; c < ch; c++) {
				const float x = io[c][i];
				float a, b;
				os[c].up(x, a, b);
				const float y = os[c].down((float)shape(a * d), (float)shape(b * d));
				double w = toneSt[c].tick(tone, y * comp);
				w -= dc[c].lp(w, dcG); // the tube curve is asymmetric and adds DC
				dry[c].write(x);
				io[c][i] = (float)(dry[c].read(Halfband2x::kLatency + 1) * (1.0 - m) + w * m);
			}
		}
	}

private:
	inline double shape(double x) const
	{
		switch (type) {
		case 1: // tube: softer on the negative half
			return x >= 0 ? std::tanh(x) : std::tanh(0.7 * x) / 0.7 * 0.85;
		case 2: { // soft clip
			const double u = clampd(x, -1.0, 1.0);
			return 1.5 * (u - u * u * u / 3.0);
		}
		default: // tape
			return std::tanh(x);
		}
	}
	double sr = 48000, dcG = 0;
	int ch = 2, type = 0;
	Smooth drive, mix;
	SvfCoef tone;
	SvfState toneSt[kMaxCh];
	OnePole dc[kMaxCh];
	Halfband2x os[kMaxCh];
	DelayLine dry[kMaxCh];
};

// ---------------------------------------------------------------- Reverb (8-line FDN)
class Reverb : public Module {
	enum { TYPE, SIZE, DECAY, PRE, DAMP, LC, HC, MIX };
	static constexpr int NL = 8, NAP = 4;

public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
		pre.resize((int)(0.21 * sr));
		for (auto &l : line)
			l.resize((int)(0.17 * sr));
		for (auto &a : ap)
			a.resize((int)(0.03 * sr));
		mix.coef = coefMs(20, sr);
	}
	void setParams(const double *v) override
	{
		static const double base[3][NL] = {{10.3, 12.7, 15.1, 17.9, 20.5, 23.3, 25.9, 28.7},  // room
						   {9.1, 11.3, 13.7, 16.9, 19.3, 22.7, 26.9, 31.1},   // plate
						   {31.3, 37.1, 43.7, 49.9, 56.3, 62.9, 71.3, 79.9}}; // hall
		static const double apMs[NAP] = {4.7, 3.5, 12.7, 9.3};
		const int type = (int)clampd(v[TYPE], 0, 2);
		const double scale = 0.5 + v[SIZE] / 100.0 * 1.5;
		decay = v[DECAY];
		for (int i = 0; i < NL; i++) {
			len[i] = std::min((int)(base[type][i] * scale * 0.001 * sr), line[i].capacity() - 2);
			fb[i] = std::pow(10.0, -3.0 * len[i] / (decay * sr));
		}
		for (int i = 0; i < NAP; i++)
			apLen[i] = std::max(1, (int)(apMs[i] * (type == 2 ? 1.6 : 1.0) * 0.001 * sr));
		apG = type == 1 ? 0.7 : 0.6;
		preLen = (int)(v[PRE] * 0.001 * sr);
		damp = 1.0 - v[DAMP] / 100.0 * 0.85;
		lc = svfCoef(Svf::HighPass, v[LC], 0.7071, 0, sr);
		hc = svfCoef(Svf::LowPass, v[HC], 0.7071, 0, sr);
		mix.set(v[MIX] / 100.0);
		tail = decay + v[PRE] * 0.001 + 0.1;
	}
	double tailSeconds() const override { return tail; }
	void setInputMuted(bool m) override { muted = m; }
	void process(float **io, int n, const Ctx &) override
	{
		const int outs = std::min(ch, 2);
		for (int i = 0; i < n; i++) {
			double in = 0;
			if (!muted) {
				for (int c = 0; c < outs; c++)
					in += io[c][i];
				in /= outs;
			}
			pre.write((float)in);
			double x = pre.read(preLen + 1);
			for (int a = 0; a < NAP; a++) { // input diffusion
				const double d = ap[a].read(apLen[a]);
				const double w = x + d * apG;
				ap[a].write((float)w);
				x = d - w * apG;
			}
			double o[NL], s = 0;
			for (int k = 0; k < NL; k++) {
				lpf[k] += (line[k].read(len[k]) - lpf[k]) * damp;
				o[k] = lpf[k] * fb[k];
				s += o[k];
			}
			s *= 2.0 / NL; // Householder feedback matrix
			double wl = 0, wr = 0;
			for (int k = 0; k < NL; k++) {
				line[k].write((float)(x + o[k] - s));
				wl += (k & 1) ? -o[k] : o[k];
				wr += (k & 2) ? -o[k] : o[k];
			}
			double wet[2] = {wl * 0.35, wr * 0.35};
			double dg, wg;
			mixLaw(mix.next(), dg, wg);
			if (outs == 1)
				wet[0] = (wet[0] + wet[1]) * 0.5;
			for (int c = 0; c < outs; c++) {
				const double w = hcSt[c].tick(hc, lcSt[c].tick(lc, wet[c]));
				io[c][i] = (float)(io[c][i] * dg + w * wg);
			}
		}
	}

private:
	double sr = 48000, decay = 1.4, apG = 0.6, damp = 0.5, tail = 1.5;
	int ch = 2, preLen = 0, len[NL] = {}, apLen[NAP] = {};
	double fb[NL] = {}, lpf[NL] = {};
	bool muted = false;
	Smooth mix;
	DelayLine pre, line[NL], ap[NAP];
	SvfCoef lc, hc;
	SvfState lcSt[2], hcSt[2];
};

// ---------------------------------------------------------------- Delay
class Delay : public Module {
	enum { TIME, FB, PP, LC, HC, MIX };

public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
		for (auto &l : line)
			l.resize((int)(2.05 * sr));
		time.coef = coefMs(60, sr);
		mix.coef = coefMs(20, sr);
	}
	void setParams(const double *v) override
	{
		time.set(v[TIME] * 0.001 * sr);
		fb = v[FB] / 100.0;
		pingPong = v[PP] > 0.5 && ch >= 2;
		lc = svfCoef(Svf::HighPass, v[LC], 0.7071, 0, sr);
		hc = svfCoef(Svf::LowPass, v[HC], 0.7071, 0, sr);
		mix.set(v[MIX] / 100.0);
		// time for the echoes to fall 60 dB
		tail = std::min(10.0, v[TIME] * 0.001 * (fb > 0.001 ? 3.0 / -std::log10(fb) : 1.0) + 0.1);
	}
	double tailSeconds() const override { return tail; }
	void setInputMuted(bool m) override { muted = m; }
	void process(float **io, int n, const Ctx &) override
	{
		const int outs = std::min(ch, 2);
		for (int i = 0; i < n; i++) {
			const double t = std::max(1.0, time.next());
			double dg, wg;
			mixLaw(mix.next(), dg, wg);
			double in[2] = {0, 0}, d[2], f[2];
			for (int c = 0; c < outs; c++) {
				in[c] = muted ? 0.0 : io[c][i];
				d[c] = line[c].readFrac(t);
				f[c] = hcSt[c].tick(hc, lcSt[c].tick(lc, d[c])) * fb;
			}
			if (pingPong) {
				line[0].write((float)((in[0] + in[1]) * 0.5 + f[1]));
				line[1].write((float)f[0]);
			} else {
				for (int c = 0; c < outs; c++)
					line[c].write((float)(in[c] + f[c]));
			}
			for (int c = 0; c < outs; c++)
				io[c][i] = (float)(io[c][i] * dg + d[c] * wg);
		}
	}

private:
	double sr = 48000, fb = 0.25, tail = 1;
	int ch = 2;
	bool pingPong = false, muted = false;
	Smooth time, mix;
	DelayLine line[2];
	SvfCoef lc, hc;
	SvfState lcSt[2], hcSt[2];
};

// ---------------------------------------------------------------- Output
// aux[0], aux[1]: peak L/R in dBFS. aux[2]: momentary LUFS. aux[3]: integrated LUFS.
class Output : public Module {
	enum { GAIN, WIDTH, BAL, MONO, MIX, TARGET, AUTO };
	static constexpr int kBins = 800; // -70 .. +10 LUFS in 0.1 LU steps

public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
		g.coef = width.coef = mix.coef = balL.coef = balR.coef = coefMs(20, sr);
		levelC = coefMs(3000, sr);
		autoC = coefMs(1000, sr, 64);
		// ITU-R BS.1770 K-weighting: high shelf, then high-pass
		for (int c = 0; c < ch; c++) {
			Biquad &s = shelf[c], &h = hpf[c];
			double K = std::tan(kPi * 1681.974450955533 / sr), Q = 0.7071752369554196;
			const double Vh = std::pow(10.0, 3.999843853973347 / 20.0),
				     Vb = std::pow(Vh, 0.4996667741545416);
			double a0 = 1.0 + K / Q + K * K;
			s.b0 = (Vh + Vb * K / Q + K * K) / a0;
			s.b1 = 2.0 * (K * K - Vh) / a0;
			s.b2 = (Vh - Vb * K / Q + K * K) / a0;
			s.a1 = 2.0 * (K * K - 1.0) / a0;
			s.a2 = (1.0 - K / Q + K * K) / a0;
			K = std::tan(kPi * 38.13547087602444 / sr);
			Q = 0.5003270373238773;
			a0 = 1.0 + K / Q + K * K;
			h.b0 = 1;
			h.b1 = -2;
			h.b2 = 1;
			h.a1 = 2.0 * (K * K - 1.0) / a0;
			h.a2 = (1.0 - K / Q + K * K) / a0;
		}
		hop = (int)(0.1 * sr);
		hist.assign(kBins, 0);
		aux[2].store(-70.0f);
		aux[3].store(-70.0f);
	}
	void setParams(const double *v) override
	{
		g.set(dbToLin(v[GAIN]));
		width.set(v[WIDTH] / 100.0);
		const double b = v[BAL] / 100.0;
		balL.set(b > 0 ? 1.0 - b : 1.0);
		balR.set(b < 0 ? 1.0 + b : 1.0);
		mono = v[MONO] > 0.5;
		mix.set(v[MIX] / 100.0);
		autoGain = v[AUTO] > 0.5;
	}
	void process(float **io, int n, const Ctx &ctx) override
	{
		float pk[2] = {0, 0};
		for (int i = 0; i < n; i++) {
			const double gg = g.next(), m = mix.next(), w = width.next();
			const double bl = balL.next(), br = balR.next();
			double x[kMaxCh];
			for (int c = 0; c < ch; c++)
				x[c] = io[c][i] * m + (ctx.dry ? ctx.dry[c][i] * (1.0 - m) : 0.0);
			if (ch == 2) {
				const double mid = (x[0] + x[1]) * 0.5, side = mono ? 0.0 : (x[0] - x[1]) * 0.5 * w;
				x[0] = (mid + side) * bl;
				x[1] = (mid - side) * br;
			}
			// Auto gain: bring the chain's output back to the loudness of its input.
			if (ctx.dry) {
				double dp = 0, wp = 0;
				for (int c = 0; c < ch; c++) {
					dp += (double)ctx.dry[c][i] * ctx.dry[c][i];
					wp += x[c] * x[c];
				}
				dryMs += (dp - dryMs) * levelC;
				wetMs += (wp - wetMs) * levelC;
			}
			if ((autoTick++ & 63) == 0) {
				// only follow the level while there is signal, so silence doesn't swing the gain
				double target = 0;
				if (autoGain && dryMs > 1e-6 * ch && wetMs > 1e-12)
					target = clampd(10.0 * std::log10(dryMs / wetMs), -12, 12);
				else if (autoGain)
					target = autoDb;
				autoDb += (target - autoDb) * autoC;
				autoLin = dbToLin(autoDb);
			}
			double p = 0;
			for (int c = 0; c < ch; c++) {
				const double y = x[c] * gg * autoLin;
				io[c][i] = (float)y;
				if (c < 2)
					pk[c] = std::max(pk[c], (float)std::fabs(y));
				const double k = hpf[c].tick(shelf[c].tick(y));
				p += k * k;
			}
			acc += p;
			if (++accN >= hop)
				loudnessBlock();
		}
		aux[0].store((float)linToDb(pk[0]));
		aux[1].store((float)linToDb(ch > 1 ? pk[1] : pk[0]));
	}

private:
	void loudnessBlock()
	{
		blocks[bpos] = acc / accN;
		bpos = (bpos + 1) & 3;
		acc = 0;
		accN = 0;
		const double e = (blocks[0] + blocks[1] + blocks[2] + blocks[3]) / 4.0; // 400 ms window
		const double l = -0.691 + 10.0 * std::log10(std::max(e, 1e-12));
		aux[2].store((float)std::max(l, -70.0));
		if (l > -70.0) { // absolute gate
			const int bin = (int)clampd((l + 70.0) * 10.0, 0, kBins - 1);
			hist[(size_t)bin]++;
		}
		// relative gate: 10 LU below the mean of everything above the absolute gate
		double sum = 0, cnt = 0;
		for (int b = 0; b < kBins; b++) {
			sum += hist[(size_t)b] * std::pow(10.0, ((b + 0.5) / 10.0 - 70.0 + 0.691) / 10.0);
			cnt += hist[(size_t)b];
		}
		if (cnt < 1)
			return;
		const double gate = -0.691 + 10.0 * std::log10(sum / cnt) - 10.0;
		sum = cnt = 0;
		for (int b = (int)clampd((gate + 70.0) * 10.0, 0, kBins - 1); b < kBins; b++) {
			sum += hist[(size_t)b] * std::pow(10.0, ((b + 0.5) / 10.0 - 70.0 + 0.691) / 10.0);
			cnt += hist[(size_t)b];
		}
		if (cnt >= 1)
			aux[3].store((float)(-0.691 + 10.0 * std::log10(sum / cnt)));
	}

	double sr = 48000, acc = 0, blocks[4] = {};
	double levelC = 0, autoC = 0, dryMs = 0, wetMs = 0, autoDb = 0, autoLin = 1;
	unsigned autoTick = 0;
	int ch = 2, hop = 4800, accN = 0, bpos = 0;
	bool mono = false, autoGain = false;
	Smooth g, width, mix, balL, balR;
	Biquad shelf[kMaxCh], hpf[kMaxCh];
	std::vector<unsigned> hist;
};

std::shared_ptr<Module> makeTrim()
{
	return std::make_shared<Trim>();
}
std::shared_ptr<Module> makeSaturation()
{
	return std::make_shared<Saturation>();
}
std::shared_ptr<Module> makeReverb()
{
	return std::make_shared<Reverb>();
}
std::shared_ptr<Module> makeDelay()
{
	return std::make_shared<Delay>();
}
std::shared_ptr<Module> makeOutput()
{
	return std::make_shared<Output>();
}

} // namespace px
