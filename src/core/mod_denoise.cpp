// Non-AI noise reduction: STFT spectral gating with a learned or adaptive noise profile.
#include "px_module.hpp"
#include "px_params.hpp"

namespace px {

class Denoise : public Module {
	enum { RED, SENS, SMOOTH, ADAPT, LEARN };

public:
	void prepare(double sampleRate, int channels) override
	{
		sr = sampleRate;
		ch = channels;
		N = sr > 50000 ? 1024 : 512; // about 10.7 ms at 48 kHz
		hop = N / 4;
		bins = N / 2 + 1;
		fft.init(N);
		win.resize((size_t)N);
		for (int i = 0; i < N; i++)
			win[(size_t)i] = (float)std::sqrt(0.5 - 0.5 * std::cos(2.0 * kPi * i / N));
		for (int c = 0; c < ch; c++) {
			in[c].assign((size_t)N, 0.0f);
			acc[c].assign((size_t)N, 0.0f);
			out[c].assign((size_t)hop, 0.0f);
			spec[c].resize((size_t)N);
		}
		power.assign((size_t)bins, 0.0f);
		smoothP.assign((size_t)bins, 0.0f);
		sig.assign((size_t)bins, 0.0f);
		noise.assign((size_t)bins, 0.0f);
		learnAcc.assign((size_t)bins, 0.0f);
		gain.assign((size_t)bins, 1.0f);
		raw.assign((size_t)bins, 1.0f);
		// the adaptive estimate may double in about 1.5 seconds
		rise = (float)std::pow(2.0, 1.0 / (1.5 * sr / hop));
	}
	void setParams(const double *v) override
	{
		floorG = (float)dbToLin(-v[RED]);
		over = (float)(1.0 + v[SENS] / 100.0 * 5.0);
		hold = (float)(0.3 + v[SMOOTH] / 100.0 * 0.68);
		adaptive = v[ADAPT] > 0.5;
		const int token = (int)v[LEARN];
		if (token != learnToken) {
			if (learnToken >= 0) { // a changed token means "Learn noise" was pressed
				learnLeft = learnTotal = (int)(2.0 * sr / hop);
				std::fill(learnAcc.begin(), learnAcc.end(), 0.0f);
			}
			learnToken = token;
		}
	}
	int latency() const override { return N; }
	void process(float **io, int n, const Ctx &) override
	{
		for (int i = 0; i < n; i++) {
			for (int c = 0; c < ch; c++) {
				in[c][(size_t)(N - hop + pos)] = io[c][i];
				io[c][i] = out[c][(size_t)pos];
			}
			if (++pos == hop) {
				frame();
				pos = 0;
			}
		}
		gr.store((float)linToDb(avgGain));
	}

private:
	void frame()
	{
		std::fill(power.begin(), power.end(), 0.0f);
		for (int c = 0; c < ch; c++) {
			for (int k = 0; k < N; k++)
				spec[c][(size_t)k] = in[c][(size_t)k] * win[(size_t)k];
			fft.run(spec[c].data(), false);
			for (int k = 0; k < bins; k++)
				power[(size_t)k] += std::norm(spec[c][(size_t)k]) / ch;
		}

		if (learnLeft > 0) {
			for (int k = 0; k < bins; k++)
				learnAcc[(size_t)k] += power[(size_t)k];
			if (--learnLeft == 0) {
				for (int k = 0; k < bins; k++)
					noise[(size_t)k] = learnAcc[(size_t)k] / learnTotal;
			}
		}
		double gsum = 0;
		for (int k = 0; k < bins; k++) {
			const size_t i = (size_t)k;
			if (frames < 8) { // warm-up: the first windows are partly empty, so don't trust their minimum
				smoothP[i] = power[i];
				noise[i] = power[i];
				sig[i] = power[i];
				raw[i] = 1.0f;
				continue;
			}
			smoothP[i] += (power[i] - smoothP[i]) * 0.1f;
			if (adaptive && learnLeft == 0) {
				// minimum tracking: follow the quietest recent level in each bin
				// (drop at once, creep back up slowly)
				if (noise[i] <= 0 || smoothP[i] < noise[i])
					noise[i] = smoothP[i];
				else
					noise[i] = std::min(noise[i] * rise, smoothP[i]);
			}
			// Signal estimate: quick to open on speech, slow to close (the Smoothing control).
			sig[i] += (power[i] - sig[i]) * (power[i] > sig[i] ? 0.6f : 1.0f - hold);
			// Minimum tracking reads about 3 dB low on noise, so the adaptive estimate is doubled.
			const float n = noise[i] * (adaptive ? 2.0f : 1.0f);
			raw[i] = std::sqrt(std::max(1.0f - over * n / (sig[i] + 1e-20f), floorG * floorG));
		}
		for (int k = 0; k < bins; k++) {
			const size_t i = (size_t)k;
			// averaging over neighbouring bins cuts musical noise
			const float a = raw[(size_t)std::max(k - 1, 0)], b = raw[(size_t)std::min(k + 1, bins - 1)];
			gain[i] = (a + raw[i] + b) / 3.0f;
			gsum += gain[i];
		}
		avgGain = gsum / bins;
		if (frames < 8)
			frames++;

		for (int c = 0; c < ch; c++) {
			for (int k = 0; k < bins; k++) {
				spec[c][(size_t)k] *= gain[(size_t)k];
				if (k > 0 && k < N / 2)
					spec[c][(size_t)(N - k)] = std::conj(spec[c][(size_t)k]);
			}
			fft.run(spec[c].data(), true);
			// sqrt-Hann analysis and synthesis at 75% overlap sum to 2
			for (int k = 0; k < N; k++)
				acc[c][(size_t)k] += spec[c][(size_t)k].real() * win[(size_t)k] * 0.5f;
			std::memcpy(out[c].data(), acc[c].data(), sizeof(float) * (size_t)hop);
			std::memmove(acc[c].data(), acc[c].data() + hop, sizeof(float) * (size_t)(N - hop));
			std::fill(acc[c].begin() + (N - hop), acc[c].end(), 0.0f);
			std::memmove(in[c].data(), in[c].data() + hop, sizeof(float) * (size_t)(N - hop));
		}
	}

	double sr = 48000, avgGain = 1;
	int ch = 2, N = 512, hop = 128, bins = 257, pos = 0;
	int learnToken = -1, learnLeft = 0, learnTotal = 1, frames = 0;
	bool adaptive = true;
	float floorG = 0.25f, over = 2, hold = 0.6f, rise = 1.0006f;
	Fft fft;
	std::vector<float> win, power, smoothP, sig, noise, learnAcc, gain, raw;
	std::vector<float> in[kMaxCh], acc[kMaxCh], out[kMaxCh];
	std::vector<std::complex<float>> spec[kMaxCh];
};

std::shared_ptr<Module> makeDenoise()
{
	return std::make_shared<Denoise>();
}

std::shared_ptr<Module> makeEq();
std::shared_ptr<Module> makeGate();
std::shared_ptr<Module> makeLeveler();
std::shared_ptr<Module> makeDeEsser();
std::shared_ptr<Module> makeCompressor();
std::shared_ptr<Module> makeMultiband();
std::shared_ptr<Module> makeLimiter();
std::shared_ptr<Module> makeTrim();
std::shared_ptr<Module> makeSaturation();
std::shared_ptr<Module> makeReverb();
std::shared_ptr<Module> makeDelay();
std::shared_ptr<Module> makeOutput();
std::shared_ptr<Module> makePitch();

std::shared_ptr<Module> createModule(const std::string &type)
{
	if (type == "trim")
		return makeTrim();
	if (type == "denoise")
		return makeDenoise();
	if (type == "gate")
		return makeGate();
	if (type == "lvl")
		return makeLeveler();
	if (type == "eq")
		return makeEq();
	if (type == "deess")
		return makeDeEsser();
	if (type == "pitch")
		return makePitch();
	if (type == "comp")
		return makeCompressor();
	if (type == "mb")
		return makeMultiband();
	if (type == "sat")
		return makeSaturation();
	if (type == "verb")
		return makeReverb();
	if (type == "delay")
		return makeDelay();
	if (type == "lim")
		return makeLimiter();
	if (type == "out")
		return makeOutput();
	return nullptr;
}

} // namespace px
