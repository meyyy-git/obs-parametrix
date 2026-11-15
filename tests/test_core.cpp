// Core tests. Plain asserts, no framework. Usage: px_tests [presets-dir]
#include "px_chain.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace px;

static int failures = 0, checks = 0;
#define CHECK(cond, ...) \
	do { \
		checks++; \
		if (!(cond)) { \
			failures++; \
			std::printf("FAIL %s:%d  %s  -> ", __FILE__, __LINE__, #cond); \
			std::printf(__VA_ARGS__); \
			std::printf("\n"); \
		} \
	} while (0)
#define NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol), "%s = %.3f, expected %.3f +/- %.3f", #a, (double)(a), (double)(b), (double)(tol))

static const double SR = 48000;

struct Rig {
	MapStore st;
	Chain chain;
	int ch = 1;
	// one module "m1" of the given type
	explicit Rig(const std::string &type, int channels = 1) : ch(channels)
	{
		st.set("chain", Val::str("m1"));
		st.set("m1.type", Val::str(type));
	}
	Rig() = default;
	void num(const std::string &k, double v) { st.set(k, Val::num(v)); }
	void flag(const std::string &k, bool v) { st.set(k, Val::boolean(v)); }
	void apply() { chain.configure(readChain(st), SR, ch); }
	void run(std::vector<std::vector<float>> &x)
	{
		const int n = (int)x[0].size();
		for (int off = 0; off < n; off += 480) {
			float *p[kMaxCh];
			for (int c = 0; c < ch; c++)
				p[c] = x[(size_t)c].data() + off;
			chain.process(p, ch, std::min(480, n - off));
		}
	}
	std::vector<float> run(std::vector<float> mono)
	{
		std::vector<std::vector<float>> x((size_t)ch, mono);
		run(x);
		return x[0];
	}
};

static std::vector<float> sine(double f, double amp, double seconds)
{
	std::vector<float> x((size_t)(seconds * SR));
	for (size_t i = 0; i < x.size(); i++)
		x[i] = (float)(amp * std::sin(2 * kPi * f * i / SR));
	return x;
}
static double rmsDb(const std::vector<float> &x, double fromS, double toS)
{
	double s = 0;
	const size_t a = (size_t)(fromS * SR), b = std::min(x.size(), (size_t)(toS * SR));
	for (size_t i = a; i < b; i++)
		s += (double)x[i] * x[i];
	return 10 * std::log10(std::max(s / std::max<size_t>(1, b - a), 1e-30));
}
static double peak(const std::vector<float> &x, size_t from = 0)
{
	double p = 0;
	for (size_t i = from; i < x.size(); i++)
		p = std::max(p, (double)std::fabs(x[i]));
	return p;
}
static bool finite(const std::vector<float> &x)
{
	for (float v : x)
		if (!std::isfinite(v))
			return false;
	return true;
}
// gain in dB of a rig at frequency f, measured after the filter settles
static double gainAt(Rig &r, double f)
{
	const std::vector<float> in = sine(f, 0.25, 1.0);
	const std::vector<float> out = r.run(in);
	return rmsDb(out, 0.5, 1.0) - rmsDb(in, 0.5, 1.0);
}
static float noise(unsigned &seed)
{
	seed = seed * 1664525u + 1013904223u;
	return (float)((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}

static void eqOneBand(Rig &r, int type, double f, double g, double q, int slope = 1)
{
	r.num("m1.nbands", 1);
	r.num("m1.b0.type", type);
	r.num("m1.b0.freq_hz", f);
	r.num("m1.b0.gain_db", g);
	r.num("m1.b0.q", q);
	r.num("m1.b0.slope", slope);
}

static void testJsonAndRegistry()
{
	FlatMap m;
	std::string err;
	CHECK(parseFlatJson("{ \"chain\": \"m1,m2\", \"m1.on\": true, \"m1.gain_db\": -3.5, \"n\": \"a\\\"b\" }", m,
			    &err),
	      "%s", err.c_str());
	CHECK(m["chain"].s == "m1,m2" && m["m1.on"].type == Val::Bool && m["m1.gain_db"].n == -3.5 &&
		      m["n"].s == "a\"b",
	      "parsed values wrong");
	FlatMap back;
	CHECK(parseFlatJson(toFlatJson(m), back) && back.size() == m.size(), "round trip");
	FlatMap nested;
	CHECK(!parseFlatJson("{\"a\": {\"b\": 1}}", nested, &err), "nested objects must be rejected");

	for (const ModuleType &t : registry())
		CHECK(createModule(t.type) != nullptr, "no module for type %s", t.type.c_str());
	CHECK(findType("eq")->params.size() == (size_t)(kEqFirstBand + kEqBands * kEqBandParams), "eq param count");
	CHECK(findType("mb")->params.size() == (size_t)(kMbFirstBand + kMbBands * kMbBandParams + 1), "mb param count");
	CHECK(findType("eq")->index("b3.freq_hz") == kEqFirstBand + 3 * kEqBandParams + EQ_FREQ, "eq layout");
}

static void testEq()
{
	{
		Rig r("eq");
		eqOneBand(r, EQT_BELL, 1000, 6, 1);
		r.apply();
		NEAR(gainAt(r, 1000), 6.0, 0.1);
		NEAR(gainAt(r, 60), 0.0, 0.3);
		NEAR(gainAt(r, 12000), 0.0, 0.3);
	}
	{
		Rig r("eq");
		eqOneBand(r, EQT_BELL, 3000, -9, 2);
		r.apply();
		NEAR(gainAt(r, 3000), -9.0, 0.1);
	}
	{
		Rig r("eq");
		eqOneBand(r, EQT_LOWSHELF, 200, 6, 0.7071);
		r.apply();
		NEAR(gainAt(r, 25), 6.0, 0.3);
		NEAR(gainAt(r, 5000), 0.0, 0.2);
	}
	{
		Rig r("eq");
		eqOneBand(r, EQT_HIGHSHELF, 4000, -6, 0.7071);
		r.apply();
		NEAR(gainAt(r, 18000), -6.0, 0.4);
		NEAR(gainAt(r, 100), 0.0, 0.2);
	}
	// Low cut at 1 kHz: every slope must match the Butterworth magnitude at 500 Hz and 1 kHz.
	for (int s = 0; s < 8; s++) {
		Rig r("eq");
		eqOneBand(r, EQT_LOWCUT, 1000, 0, 0.7071, s);
		r.apply();
		const int order = kEqSlopes[s] / 6;
		NEAR(gainAt(r, 1000), -3.01, 0.3);
		NEAR(gainAt(r, 500), -10 * std::log10(1 + std::pow(2.0, 2.0 * order)), 0.6);
		NEAR(gainAt(r, 8000), 0.0, 0.2);
	}
	{
		Rig r("eq");
		eqOneBand(r, EQT_HIGHCUT, 2000, 0, 0.7071, 3);
		r.apply();
		NEAR(gainAt(r, 2000), -3.01, 0.3);
		NEAR(gainAt(r, 4000), -10 * std::log10(1 + std::pow(2.0, 8.0)), 0.8);
	}
	{
		Rig r("eq");
		eqOneBand(r, EQT_NOTCH, 1000, 0, 4);
		r.apply();
		CHECK(gainAt(r, 1000) < -40, "notch depth %.1f", gainAt(r, 1000));
		NEAR(gainAt(r, 100), 0.0, 0.2);
	}
	{
		Rig r("eq");
		eqOneBand(r, EQT_TILT, 1000, 6, 0.7071);
		r.apply();
		NEAR(gainAt(r, 30), -3.0, 0.4);
		NEAR(gainAt(r, 16000), 3.0, 0.5);
	}
	{ // flat tilt: +10 dB across the spectrum, pivot at 1 kHz
		Rig r("eq");
		eqOneBand(r, EQT_FLATTILT, 1000, 10, 1);
		r.apply();
		NEAR(gainAt(r, 1000), 0.0, 0.6);
		CHECK(gainAt(r, 8000) > 2.0 && gainAt(r, 125) < -2.0, "flat tilt direction");
	}
	{ // the response curve drawn by the editor must match what the audio path does
		struct Case {
			int type;
			double f, g, q;
			int slope;
		};
		for (const Case &c : {Case{EQT_BELL, 1000, 6, 1, 1}, Case{EQT_LOWSHELF, 200, -5, 0.7071, 1},
				      Case{EQT_HIGHSHELF, 5000, 4, 0.7071, 1}, Case{EQT_LOWCUT, 300, 0, 0.7071, 2},
				      Case{EQT_HIGHCUT, 3000, 0, 1.4, 5}, Case{EQT_TILT, 1000, 6, 0.7071, 1},
				      Case{EQT_FLATTILT, 1000, 8, 1, 1}, Case{EQT_BANDPASS, 800, 0, 2, 1}}) {
			Rig r("eq");
			eqOneBand(r, c.type, c.f, c.g, c.q, c.slope);
			r.apply();
			double p[kEqBandParams] = {};
			p[EQ_TYPE] = c.type;
			p[EQ_FREQ] = c.f;
			p[EQ_Q] = c.q;
			p[EQ_SLOPE] = c.slope;
			for (double f : {150.0, 700.0, 1000.0, 4000.0})
				NEAR(eqBandDb(p, c.g, SR, f), gainAt(r, f), 0.15);
		}
	}
	{ // analyzer taps return the newest samples of the module's input and output
		Rig r("eq");
		eqOneBand(r, EQT_BELL, 1000, 12, 1);
		r.apply();
		r.run(sine(1000, 0.1, 0.5));
		float in[1024], out[1024];
		auto m = r.chain.module("m1");
		CHECK(m->tap(0, in, 1024) == 1024 && m->tap(1, out, 1024) == 1024, "tap length");
		double pi = 0, po = 0;
		for (int i = 0; i < 1024; i++) {
			pi = std::max(pi, (double)std::fabs(in[i]));
			po = std::max(po, (double)std::fabs(out[i]));
		}
		NEAR(20 * std::log10(po / pi), 12.0, 0.2);
	}
	{ // linear phase: same magnitude as the normal EQ, a symmetric impulse response, known latency
		Rig r("eq");
		eqOneBand(r, EQT_BELL, 1000, 6, 1);
		r.num("m1.phase", 1);
		r.apply();
		NEAR(gainAt(r, 1000), 6.0, 0.3);
		NEAR(gainAt(r, 100), 0.0, 0.3);
		NEAR(gainAt(r, 8000), 0.0, 0.3);
		const int lat = r.chain.latencySamples();
		CHECK(lat == 2304, "linear-phase latency %d", lat);
		Rig q("eq");
		eqOneBand(q, EQT_LOWSHELF, 300, 6, 0.7071);
		q.num("m1.phase", 1);
		q.apply();
		std::vector<float> imp(8192, 0.0f);
		imp[100] = 1.0f;
		const std::vector<float> h = q.run(imp);
		double asym = 0, pk = 0;
		for (int i = 1; i < 1500; i++) {
			asym = std::max(asym,
					(double)std::fabs(h[(size_t)(100 + lat + i)] - h[(size_t)(100 + lat - i)]));
			pk = std::max(pk, (double)std::fabs(h[(size_t)(100 + lat + i)]));
		}
		CHECK(std::fabs(h[(size_t)(100 + lat)]) > 0.5 && asym < 1e-4 && pk < std::fabs(h[(size_t)(100 + lat)]),
		      "impulse response not symmetric around the latency: centre %.3f, asymmetry %.6f",
		      h[(size_t)(100 + lat)], asym);
		// a steep low cut still lands close to its design in linear mode
		Rig c("eq");
		eqOneBand(c, EQT_LOWCUT, 200, 0, 0.7071, 3);
		c.num("m1.phase", 1);
		c.apply();
		NEAR(gainAt(c, 200), -3.0, 1.0);
		NEAR(gainAt(c, 1000), 0.0, 0.3);
	}
	{ // a band placed on Left must not touch Right
		Rig r("eq", 2);
		eqOneBand(r, EQT_BELL, 1000, 12, 1);
		r.num("m1.b0.placement", 1);
		r.apply();
		std::vector<std::vector<float>> x(2, sine(1000, 0.1, 1.0));
		const std::vector<float> in = x[0];
		r.run(x);
		NEAR(rmsDb(x[0], 0.5, 1) - rmsDb(in, 0.5, 1), 12.0, 0.2);
		NEAR(rmsDb(x[1], 0.5, 1) - rmsDb(in, 0.5, 1), 0.0, 0.01);
	}
	{ // mid-only band on a pure side signal does nothing
		Rig r("eq", 2);
		eqOneBand(r, EQT_BELL, 1000, 12, 1);
		r.num("m1.b0.placement", 3);
		r.apply();
		std::vector<std::vector<float>> x(2, sine(1000, 0.1, 1.0));
		for (float &v : x[1])
			v = -v;
		const std::vector<float> in = x[0];
		r.run(x);
		NEAR(rmsDb(x[0], 0.5, 1) - rmsDb(in, 0.5, 1), 0.0, 0.05);
	}
	{ // dynamic band: cuts only while the band is loud
		Rig r("eq");
		eqOneBand(r, EQT_BELL, 6000, 0, 2);
		r.flag("m1.b0.dyn_on", true);
		r.num("m1.b0.dyn_threshold_db", -30);
		r.num("m1.b0.dyn_range_db", -8);
		r.apply();
		const std::vector<float> loud = sine(6000, 0.5, 1.0), quiet = sine(6000, 0.003, 1.0);
		NEAR(rmsDb(r.run(loud), 0.5, 1) - rmsDb(loud, 0.5, 1), -8.0, 0.6);
		Rig q("eq");
		eqOneBand(q, EQT_BELL, 6000, 0, 2);
		q.flag("m1.b0.dyn_on", true);
		q.num("m1.b0.dyn_threshold_db", -30);
		q.num("m1.b0.dyn_range_db", -8);
		q.apply();
		NEAR(rmsDb(q.run(quiet), 0.5, 1) - rmsDb(quiet, 0.5, 1), 0.0, 0.2);
	}
}

static void testDynamics()
{
	{ // compressor static curve, RMS detector: -13 dB RMS in, 7 dB over, 4:1 -> 5.25 dB reduction
		Rig r("comp");
		r.num("m1.threshold_db", -20);
		r.num("m1.ratio", 4);
		r.num("m1.knee_db", 0);
		r.num("m1.makeup_db", 0);
		r.num("m1.detector", 1);
		r.apply();
		const std::vector<float> in = sine(1000, dbToLin(-10), 2.0);
		NEAR(rmsDb(r.run(in), 1.5, 2.0) - rmsDb(in, 1.5, 2.0), -5.25, 0.3);
	}
	{ // below threshold nothing happens
		Rig r("comp");
		r.num("m1.threshold_db", -20);
		r.num("m1.makeup_db", 0);
		r.num("m1.knee_db", 0);
		r.apply();
		const std::vector<float> in = sine(1000, dbToLin(-40), 1.0);
		NEAR(rmsDb(r.run(in), 0.5, 1.0) - rmsDb(in, 0.5, 1.0), 0.0, 0.05);
	}
	{ // limiter: the ceiling is never exceeded, whatever comes in
		Rig r("lim", 2);
		r.num("m1.ceiling_db", -1);
		r.num("m1.release_ms", 20);
		r.apply();
		unsigned seed = 7;
		std::vector<std::vector<float>> x(2, std::vector<float>((size_t)SR * 2));
		for (size_t i = 0; i < x[0].size(); i++) {
			const float burst = (i / 4000) % 3 == 0 ? 4.0f : 0.3f;
			x[0][i] = noise(seed) * burst;
			x[1][i] = noise(seed) * burst * 0.7f;
		}
		x[0][5000] = 30.0f; // single-sample spike
		r.run(x);
		const double ceil = dbToLin(-1);
		CHECK(peak(x[0]) <= ceil + 1e-6 && peak(x[1]) <= ceil + 1e-6, "limiter overshoot: %.6f / %.6f > %.6f",
		      peak(x[0]), peak(x[1]), ceil);
		CHECK(r.chain.latencySamples() == 72, "limiter latency %d", r.chain.latencySamples());
	}
	{ // true peak: a sine at fs/4 sampled 45 degrees off its crests hides 3 dB between samples
		for (int tp = 0; tp < 2; tp++) {
			Rig r("lim");
			r.num("m1.ceiling_db", -1);
			r.flag("m1.true_peak", tp == 1);
			r.apply();
			std::vector<float> in((size_t)SR);
			for (size_t i = 0; i < in.size(); i++)
				in[i] = (float)(1.4 * std::sin(2 * kPi * 0.25 * i + kPi / 4));
			const std::vector<float> out = r.run(in);
			// reconstruct between the samples with the same 4x oversampler
			Halfband2x a, b;
			double truePk = 0;
			for (size_t i = 0; i < out.size(); i++) {
				float y0, y1, q[4];
				a.up(out[i], y0, y1);
				b.up(y0, q[0], q[1]);
				b.up(y1, q[2], q[3]);
				if (i > 4800)
					for (float v : q)
						truePk = std::max(truePk, (double)std::fabs(v));
			}
			const double ceil = dbToLin(-1);
			if (tp)
				CHECK(truePk <= ceil * 1.03, "true peak %.3f over the ceiling %.3f", truePk, ceil);
			else
				CHECK(truePk > ceil * 1.2, "the test signal should overshoot without true peak (%.3f)",
				      truePk);
			CHECK(r.chain.latencySamples() == 72 + (tp ? 12 : 0), "limiter latency %d",
			      r.chain.latencySamples());
		}
	}
	{ // limiter leaves quiet audio alone (apart from its delay)
		Rig r("lim");
		r.apply();
		const std::vector<float> in = sine(1000, 0.1, 1.0);
		NEAR(rmsDb(r.run(in), 0.5, 1.0) - rmsDb(in, 0.5, 1.0), 0.0, 0.01);
	}
	{ // gate: closed below threshold, open above
		Rig r("gate");
		r.num("m1.threshold_db", -40);
		r.num("m1.range_db", -30);
		r.num("m1.ratio", 100);
		r.apply();
		const std::vector<float> quiet = sine(1000, dbToLin(-60), 2.0), loud = sine(1000, dbToLin(-20), 1.0);
		NEAR(rmsDb(r.run(quiet), 1.5, 2.0) - rmsDb(quiet, 1.5, 2.0), -30.0, 1.0);
		Rig o("gate");
		o.num("m1.threshold_db", -40);
		o.apply();
		NEAR(rmsDb(o.run(loud), 0.5, 1.0) - rmsDb(loud, 0.5, 1.0), 0.0, 0.1);
	}
	{ // de-esser: reduces a loud 7 kHz tone, leaves 300 Hz alone
		Rig r("deess");
		r.num("m1.threshold_db", -30);
		r.num("m1.range_db", -8);
		r.apply();
		const std::vector<float> ess = sine(12000, 0.3, 1.0);
		const double red = rmsDb(r.run(ess), 0.5, 1.0) - rmsDb(ess, 0.5, 1.0);
		CHECK(red < -4.5 && red > -8.5, "de-esser reduction %.1f dB, expected between -4.5 and -8", red);
		Rig l("deess");
		l.apply();
		const std::vector<float> low = sine(300, 0.3, 1.0);
		NEAR(rmsDb(l.run(low), 0.5, 1.0) - rmsDb(low, 0.5, 1.0), 0.0, 0.2);
	}
	{ // multiband with nothing to compress sums back flat
		for (double f : {60.0, 180.0, 700.0, 1800.0, 6000.0, 14000.0}) {
			Rig r("mb");
			for (int b = 0; b < kMbBands; b++)
				r.num("m1.b" + std::to_string(b) + ".threshold_db", 0);
			r.apply();
			NEAR(gainAt(r, f), 0.0, 0.1);
		}
	}
	{ // leveler pulls a quiet voice up toward the target, within its limit
		Rig r("lvl");
		r.num("m1.target_db", -18);
		r.num("m1.max_change_db", 6);
		r.num("m1.speed", 100);
		r.apply();
		const std::vector<float> in = sine(500, dbToLin(-30), 4.0); // -33 dB RMS
		NEAR(rmsDb(r.run(in), 3.5, 4.0) - rmsDb(in, 3.5, 4.0), 6.0, 0.3);
	}
}

static void testFx()
{
	{ // trim gain, polarity and align delay
		Rig r("trim");
		r.num("m1.gain_db", -6);
		r.flag("m1.invert", true);
		r.num("m1.align_delay_ms", 10);
		r.apply();
		std::vector<float> in((size_t)SR / 2, 0.0f);
		in[100] = 1.0f;
		const std::vector<float> out = r.run(in);
		NEAR(out[100 + 480], -dbToLin(-6), 1e-3);
		NEAR(out[100], 0.0, 1e-6);
	}
	{ // saturation stays finite and bounded, and mix 0 is the dry signal delayed
		Rig r("sat");
		r.num("m1.drive_db", 24);
		r.apply();
		const std::vector<float> out = r.run(sine(200, 1.0, 0.5));
		CHECK(finite(out) && peak(out) < 2.0, "saturation output out of range: %.3f", peak(out));
		Rig d("sat");
		d.num("m1.mix", 0);
		d.apply();
		const std::vector<float> in = sine(200, 0.5, 0.5), dry = d.run(in);
		NEAR(dry[5000 + Halfband2x::kLatency], in[5000], 1e-4);
	}
	{ // delay: an impulse comes back after the delay time
		Rig r("delay");
		r.num("m1.time_ms", 100);
		r.num("m1.feedback", 0);
		r.num("m1.mix", 50);
		r.num("m1.low_cut_hz", 20);
		r.num("m1.high_cut_hz", 20000);
		r.apply();
		std::vector<float> in((size_t)SR, 0.0f);
		in[1000] = 1.0f;
		const std::vector<float> out = r.run(in);
		NEAR(out[1000], 1.0, 1e-3);
		CHECK(std::fabs(out[1000 + 4800]) > 0.5, "no echo at 100 ms: %.3f", out[1000 + 4800]);
	}
	{ // reverb: a tail exists, decays, and never blows up
		Rig r("verb", 2);
		r.num("m1.decay_s", 1.0);
		r.num("m1.mix", 50);
		r.apply();
		std::vector<std::vector<float>> x(2, std::vector<float>((size_t)SR * 4, 0.0f));
		x[0][100] = x[1][100] = 1.0f;
		r.run(x);
		CHECK(finite(x[0]) && finite(x[1]), "reverb produced non-finite output");
		const double early = rmsDb(x[0], 0.1, 0.3), late = rmsDb(x[0], 3.0, 4.0);
		CHECK(early > -80 && late < early - 40, "reverb decay wrong: early %.1f, late %.1f", early, late);
	}
	{ // output: LUFS of a stereo 997 Hz sine at -20 dBFS is about -20
		Rig r("out", 2);
		r.apply();
		std::vector<std::vector<float>> x(2, sine(997, 0.1, 3.0));
		r.run(x);
		auto m = r.chain.module("m1");
		NEAR(m->aux[2].load(), -20.0, 0.3);
		NEAR(m->aux[3].load(), -20.0, 0.3);
	}
	{ // auto gain undoes a level change made earlier in the chain
		Rig r("trim");
		r.num("m1.gain_db", -10);
		r.st.set("chain", Val::str("m1,m2"));
		r.st.set("m2.type", Val::str("out"));
		r.flag("m2.auto_gain", true);
		r.apply();
		const std::vector<float> in = sine(400, 0.2, 14.0), out = r.run(in);
		NEAR(rmsDb(out, 13.0, 14.0) - rmsDb(in, 13.0, 14.0), 0.0, 0.7);
	}
	{ // output: mono folds a pure side signal to silence, width 0 does the same
		Rig r("out", 2);
		r.flag("m1.mono", true);
		r.apply();
		std::vector<std::vector<float>> x(2, sine(500, 0.3, 0.5));
		for (float &v : x[1])
			v = -v;
		r.run(x);
		CHECK(peak(x[0], 4800) < 1e-4, "mono did not cancel side: %.5f", peak(x[0], 4800));
	}
	{ // denoise: steady noise drops by at least 6 dB once the profile adapts
		Rig r("denoise");
		r.num("m1.reduction_db", 20);
		r.apply();
		unsigned seed = 3;
		std::vector<float> in((size_t)SR * 5);
		for (float &v : in)
			v = noise(seed) * 0.01f;
		const std::vector<float> out = r.run(in);
		CHECK(finite(out), "denoise produced non-finite output");
		CHECK(rmsDb(out, 4.0, 5.0) - rmsDb(in, 4.0, 5.0) < -6.0, "denoise reduction only %.1f dB",
		      rmsDb(out, 4.0, 5.0) - rmsDb(in, 4.0, 5.0));
		CHECK(r.chain.latencySamples() == 512, "denoise latency %d", r.chain.latencySamples());
	}
	{ // denoise learns the room, then passes a voice-level tone through with its level intact
		Rig r("denoise");
		r.apply();
		unsigned seed = 9;
		std::vector<float> in((size_t)SR * 4);
		for (size_t i = 0; i < in.size(); i++)
			in[i] = noise(seed) * 0.001f +
				(i > (size_t)SR * 2 ? (float)(0.3 * std::sin(2 * kPi * 1000 * i / SR)) : 0.0f);
		const std::vector<float> out = r.run(in);
		NEAR(rmsDb(out, 3.0, 4.0) - rmsDb(in, 3.0, 4.0), 0.0, 0.5);
	}
}

// Level of one frequency in a signal, by direct correlation over [fromS, toS).
static double toneLevel(const std::vector<float> &x, double f, double fromS, double toS)
{
	double re = 0, im = 0;
	const size_t a = (size_t)(fromS * SR), b = std::min(x.size(), (size_t)(toS * SR));
	for (size_t i = a; i < b; i++) {
		re += x[i] * std::cos(2 * kPi * f * i / SR);
		im += x[i] * std::sin(2 * kPi * f * i / SR);
	}
	return 2.0 * std::sqrt(re * re + im * im) / (double)(b - a);
}

static void testPitch()
{
	{ // a sine 47 cents sharp of A3 is pulled to 220 Hz
		Rig r("pitch");
		r.num("m1.speed_ms", 20);
		r.apply();
		const std::vector<float> in = sine(226, 0.3, 4.0), out = r.run(in);
		const double at220 = toneLevel(out, 220, 2, 4), at226 = toneLevel(out, 226, 2, 4);
		CHECK(at220 > 0.2 && at220 > 8 * at226, "pitch not corrected: 220 Hz %.3f, 226 Hz %.3f", at220, at226);
		CHECK(r.chain.latencySamples() == 960, "pitch latency %d", r.chain.latencySamples());
		auto m = r.chain.module("m1");
		NEAR(m->aux[0].load(), 69 - 12 + 12 * std::log2(226.0 / 220.0), 0.05); // detected note, A3 = MIDI 57
	}
	{ // a harmonic-rich tone 33 cents flat of D#3 lands on D#3
		const double f = 155.563 * std::pow(2.0, -33.0 / 1200.0);
		std::vector<float> in((size_t)SR * 4);
		for (size_t i = 0; i < in.size(); i++)
			for (int h = 1; h <= 8; h++)
				in[i] += (float)(0.25 / h * std::sin(2 * kPi * f * h * i / SR));
		Rig r("pitch");
		r.num("m1.speed_ms", 20);
		r.apply();
		const std::vector<float> out = r.run(in);
		const double on = toneLevel(out, 155.563, 2, 4), off = toneLevel(out, f, 2, 4);
		CHECK(on > 0.15 && on > 6 * off, "voice-like tone not corrected: on-note %.3f, original %.3f", on, off);
		NEAR(rmsDb(out, 2, 4) - rmsDb(in, 2, 4), 0.0, 1.0);
	}
	{ // an in-tune note passes through, and amount 0% leaves a detuned one alone
		Rig r("pitch");
		r.apply();
		const std::vector<float> in = sine(220, 0.3, 3.0), out = r.run(in);
		CHECK(toneLevel(out, 220, 1.5, 3) > 0.27, "in-tune note lost level: %.3f", toneLevel(out, 220, 1.5, 3));
		Rig z("pitch");
		z.num("m1.amount", 0);
		z.apply();
		const std::vector<float> det = sine(226, 0.3, 3.0), same = z.run(det);
		CHECK(toneLevel(same, 226, 1.5, 3) > 0.27, "amount 0 changed the pitch: %.3f",
		      toneLevel(same, 226, 1.5, 3));
	}
	{ // C major: a C# (277.18 Hz) snaps to C or D, never stays
		Rig r("pitch");
		r.num("m1.scale", 1);
		r.num("m1.speed_ms", 20);
		r.apply();
		const std::vector<float> in = sine(277.183 * std::pow(2.0, 20.0 / 1200.0), 0.3, 4.0), out = r.run(in);
		CHECK(toneLevel(out, 293.665, 2, 4) > 0.2, "C#+20c should move up to D in C major: %.3f",
		      toneLevel(out, 293.665, 2, 4));
	}
	{ // noise and silence never produce garbage
		Rig r("pitch", 2);
		r.apply();
		unsigned seed = 21;
		std::vector<std::vector<float>> x(2, std::vector<float>((size_t)SR * 2));
		for (size_t i = 0; i < x[0].size(); i++)
			x[0][i] = x[1][i] = i < (size_t)SR ? noise(seed) * 0.3f : 0.0f;
		r.run(x);
		CHECK(finite(x[0]) && peak(x[0]) < 1.0 && peak(x[0], (size_t)(SR * 1.2)) < 1e-4,
		      "pitch module misbehaved on noise or silence");
	}
}

static void testChain()
{
	{ // rack edits
		MapStore s;
		ensureChain(s);
		CHECK(chainIds(s).size() == 7, "default chain size %zu", chainIds(s).size());
		const std::string id = addModule(s, "verb");
		std::vector<std::string> ids = chainIds(s);
		CHECK(!id.empty() && ids.size() == 8 && ids[5] == id && ids[6] == "m6",
		      "new module goes before the limiter");
		CHECK(!limiterMisplaced(s), "limiter should still be last");
		CHECK(addModule(s, "trim").empty() && addModule(s, "out").empty() && addModule(s, "nope").empty(),
		      "locked or unknown types must be refused");
		CHECK(!removeModule(s, "m1") && !removeModule(s, "m7"), "trim and output cannot be removed");
		CHECK(!moveModule(s, "m2", -1), "nothing moves ahead of trim");
		CHECK(moveModule(s, "m6", -1) && limiterMisplaced(s),
		      "moving the limiter earlier is allowed but flagged");
		CHECK(removeModule(s, id) && chainIds(s).size() == 7, "remove");
		double dummy;
		CHECK(!s.getNum(id + ".on", dummy), "removed module keys must be erased");
		for (int i = 0; i < 20; i++)
			addModule(s, "comp");
		CHECK(chainIds(s).size() == (size_t)kMaxModules, "rack is capped at %d", kMaxModules);
	}
	{ // removing an EQ band shifts the ones above it down
		MapStore s;
		ensureChain(s);
		CHECK(removeEqBand(s, "m3", 1), "remove band");
		double n = 0, f = 0, t = -1;
		s.getNum("m3.nbands", n);
		s.getNum("m3.b1.freq_hz", f);
		s.getNum("m3.b1.type", t);
		CHECK(n == 4 && f == 320 && t == EQT_BELL, "band shift: n=%.0f f=%.0f type=%.0f", n, f, t);
		CHECK(addEqBand(s, "m3"), "add band");
		s.getNum("m3.b4.freq_hz", f);
		CHECK(f == 1000, "new band starts at 1 kHz, got %.0f", f);
	}
	{ // preset round trip
		MapStore a, b;
		ensureChain(a);
		a.set("m5.ratio", Val::num(7));
		a.set("m3.b2.dyn_sidechain", Val::str("Mic/Aux"));
		applyChain(b, captureChain(a));
		const ChainCfg ca = readChain(a), cb = readChain(b);
		bool same = ca.modules.size() == cb.modules.size() && ca.sidechains == cb.sidechains;
		for (size_t i = 0; same && i < ca.modules.size(); i++)
			same = ca.modules[i].type == cb.modules[i].type && ca.modules[i].values == cb.modules[i].values;
		CHECK(same && cb.sidechains.size() == 1, "preset round trip changed the rack");
	}
	{ // module presets replace one module and nothing else
		MapStore a, b;
		ensureChain(a);
		ensureChain(b);
		a.set("m5.ratio", Val::num(9));
		a.set("m5.auto_makeup", Val::boolean(true));
		b.set("m5.knee_db", Val::num(20));
		b.set("m2.threshold_db", Val::num(-33));
		const FlatMap preset = captureModule(a, "m5");
		CHECK(preset.count("ratio") && preset.at("_type").s == "comp" && !preset.count("type"),
		      "module preset shape");
		CHECK(applyModule(b, "m5", preset), "apply module preset");
		double ratio = 0, knee = 0, gate = 0, mk = 0;
		b.getNum("m5.ratio", ratio);
		b.getNum("m5.knee_db", knee);
		b.getNum("m2.threshold_db", gate);
		b.getNum("m5.auto_makeup", mk);
		CHECK(ratio == 9 && knee == 6 && gate == -33 && mk == 1,
		      "module preset result: ratio %.0f knee %.0f gate %.0f automk %.0f", ratio, knee, gate, mk);
		CHECK(!applyModule(b, "m2", preset), "a compressor preset must not load into a gate");
	}
	{ // toggling a module crossfades: no jump larger than the signal's own slope allows
		Rig r("trim");
		r.num("m1.gain_db", 0);
		r.st.set("chain", Val::str("m1,m2"));
		r.st.set("m2.type", Val::str("comp"));
		r.num("m2.threshold_db", 0);
		r.num("m2.makeup_db", 12);
		r.apply();
		std::vector<float> all;
		for (int k = 0; k < 6; k++) {
			r.flag("m2.on", k % 2 == 0);
			r.apply();
			const std::vector<float> part = r.run(sine(100, 0.1, 0.2));
			all.insert(all.end(), part.begin(), part.end());
		}
		double jump = 0;
		for (size_t i = 1; i < all.size(); i++)
			if (i % 9600 != 0) // each part restarts the sine at phase 0, which is continuous anyway
				jump = std::max(jump, (double)std::fabs(all[i] - all[i - 1]));
		CHECK(jump < 0.02, "bypass toggle clicked: max step %.4f", jump);
		CHECK(peak(all) > 0.3, "the compressor makeup never engaged");
	}
	{ // a bypassed reverb keeps ringing, and the dry signal still passes
		Rig r("verb");
		r.num("m1.decay_s", 2.0);
		r.num("m1.mix", 50);
		r.apply();
		unsigned seed = 11;
		std::vector<float> burst((size_t)SR / 2);
		for (float &v : burst)
			v = noise(seed) * 0.5f;
		r.run(burst);
		r.flag("m1.on", false);
		r.apply();
		const std::vector<float> tail = r.run(std::vector<float>((size_t)SR, 0.0f));
		CHECK(rmsDb(tail, 0.0, 0.3) > -60, "tail was cut on bypass: %.1f dB", rmsDb(tail, 0.0, 0.3));
		const std::vector<float> later = r.run(std::vector<float>((size_t)SR * 4, 0.0f));
		CHECK(peak(later, (size_t)SR * 3) < 1e-5, "tail never ended");
	}
	{ // removing a module mid-stream does not crash or produce garbage
		Rig r;
		ensureChain(r.st);
		r.ch = 2;
		r.apply();
		std::vector<std::vector<float>> x(2, sine(440, 0.2, 0.5));
		r.run(x);
		removeModule(r.st, "m5");
		addModule(r.st, "delay");
		r.apply();
		r.run(x);
		CHECK(finite(x[0]) && finite(x[1]) && peak(x[0]) < 4, "rack edit produced bad audio");
	}
	{ // chain mix at 0% returns the dry signal, aligned with the chain latency
		Rig r("lim");
		r.st.set("chain", Val::str("m1,m2"));
		r.st.set("m2.type", Val::str("out"));
		r.num("m2.chain_mix", 0);
		r.apply();
		const std::vector<float> in = sine(300, 0.2, 0.5), out = r.run(in);
		NEAR(out[6000 + 72], in[6000], 1e-4);
	}
}

static void testPresets(const char *dir)
{
	int n = 0;
	for (const auto &e : std::filesystem::directory_iterator(dir)) {
		if (e.path().extension() != ".json")
			continue;
		std::ifstream f(e.path());
		std::stringstream ss;
		ss << f.rdbuf();
		FlatMap m;
		std::string err;
		const std::string name = e.path().filename().string();
		CHECK(parseFlatJson(ss.str(), m, &err), "%s: %s", name.c_str(), err.c_str());
		CHECK(m.count("_name") && m.count("_category"), "%s: missing _name or _category", name.c_str());
		Rig r;
		r.ch = 2;
		applyChain(r.st, m);
		const ChainCfg cfg = readChain(r.st);
		CHECK(cfg.modules.size() >= 2 && cfg.modules.front().type == "trim" && cfg.modules.back().type == "out",
		      "%s: rack must start with trim and end with out", name.c_str());
		// every key must be a known parameter, so typos fail loudly
		for (const auto &kv : m) {
			if (kv.first[0] == '_' || kv.first == "chain")
				continue;
			const size_t dot = kv.first.find('.');
			const std::string id = kv.first.substr(0, dot), rest = kv.first.substr(dot + 1);
			const ModuleType *mt = m.count(id + ".type") ? findType(m[id + ".type"].s) : nullptr;
			CHECK(mt && (rest == "type" || rest == "on" || mt->index(rest) >= 0), "%s: unknown key %s",
			      name.c_str(), kv.first.c_str());
		}
		r.apply();
		unsigned seed = 5;
		std::vector<std::vector<float>> x(2, std::vector<float>((size_t)SR));
		for (size_t i = 0; i < x[0].size(); i++)
			x[0][i] = x[1][i] = noise(seed) * 0.2f * (float)std::fabs(std::sin(i * 0.0005));
		r.run(x);
		CHECK(finite(x[0]) && finite(x[1]) && peak(x[0]) < 8, "%s: bad audio (peak %.2f)", name.c_str(),
		      peak(x[0]));
		n++;
	}
	CHECK(n >= 24, "expected at least 24 factory presets, found %d", n);

	// factory module presets: right type, only known keys, and they load
	int nm = 0;
	for (const auto &e :
	     std::filesystem::directory_iterator(std::filesystem::path(dir).parent_path() / "module-presets")) {
		if (e.path().extension() != ".json")
			continue;
		std::ifstream f(e.path());
		std::stringstream ss;
		ss << f.rdbuf();
		FlatMap m;
		std::string err;
		const std::string name = e.path().filename().string();
		CHECK(parseFlatJson(ss.str(), m, &err), "%s: %s", name.c_str(), err.c_str());
		const ModuleType *mt = m.count("_type") ? findType(m["_type"].s) : nullptr;
		CHECK(mt && m.count("_name"), "%s: missing or unknown _type, or no _name", name.c_str());
		if (!mt)
			continue;
		for (const auto &kv : m)
			CHECK(kv.first[0] == '_' || mt->index(kv.first) >= 0, "%s: unknown key %s", name.c_str(),
			      kv.first.c_str());
		Rig r(mt->type, 2);
		CHECK(applyModule(r.st, "m1", m), "%s: does not load", name.c_str());
		r.apply();
		std::vector<std::vector<float>> x(2, sine(300, 0.2, 0.5));
		r.run(x);
		CHECK(finite(x[0]) && peak(x[0]) < 8, "%s: bad audio", name.c_str());
		nm++;
	}
	CHECK(nm >= 20, "expected at least 20 factory module presets, found %d", nm);
}

int main(int argc, char **argv)
{
	testJsonAndRegistry();
	testEq();
	testDynamics();
	testFx();
	testPitch();
	testChain();
	if (argc > 1)
		testPresets(argv[1]);
	std::printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
