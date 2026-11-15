#include "px_params.hpp"

namespace px {

const int kEqSlopes[8] = {6, 12, 18, 24, 36, 48, 72, 96};

namespace {

ParamDef F(const char *key, const char *label, double mn, double mx, double def, const char *unit, bool log = false)
{
	ParamDef p;
	p.key = key;
	p.label = label;
	p.min = mn;
	p.max = mx;
	p.def = def;
	p.unit = unit;
	p.log = log;
	return p;
}
ParamDef B(const char *key, const char *label, bool def)
{
	ParamDef p;
	p.key = key;
	p.label = label;
	p.def = def ? 1 : 0;
	p.kind = Kind::Bool;
	return p;
}
ParamDef C(const char *key, const char *label, std::vector<std::string> options, int def = 0)
{
	ParamDef p;
	p.key = key;
	p.label = label;
	p.max = (double)options.size() - 1;
	p.def = def;
	p.kind = Kind::Choice;
	p.options = std::move(options);
	return p;
}
ParamDef S(const char *key, const char *label)
{
	ParamDef p;
	p.key = key;
	p.label = label;
	p.def = -1;
	p.min = -1;
	p.max = 64;
	p.kind = Kind::Source;
	return p;
}
ParamDef I(const char *key, const char *label, int mn, int mx, int def, bool hidden)
{
	ParamDef p;
	p.key = key;
	p.label = label;
	p.min = mn;
	p.max = mx;
	p.def = def;
	p.kind = Kind::Int;
	p.hidden = hidden;
	return p;
}
ParamDef pre(const std::string &prefix, const std::string &labelPrefix, ParamDef p)
{
	p.key = prefix + p.key;
	p.label = labelPrefix + p.label;
	return p;
}

std::vector<ModuleType> build()
{
	std::vector<ModuleType> r;
	auto add = [&](const char *type, const char *name, std::vector<ParamDef> ps) -> ModuleType & {
		ModuleType m;
		m.type = type;
		m.name = name;
		m.params = std::move(ps);
		r.push_back(std::move(m));
		return r.back();
	};

	add("trim", "Trim",
	    {F("gain_db", "Gain", -24, 24, 0, "dB"), B("invert", "Invert polarity", false),
	     F("align_delay_ms", "Align delay", 0, 500, 0, "ms")})
		.first = true;

	add("denoise", "Denoise",
	    {F("reduction_db", "Reduction", 0, 40, 12, "dB"), F("sensitivity", "Sensitivity", 0, 100, 50, "%"),
	     F("smoothing", "Smoothing", 0, 100, 40, "%"), B("adaptive", "Adaptive", true),
	     I("learn", "Learn token", 0, 1 << 30, 0, true)});

	add("gate", "Gate",
	    {F("threshold_db", "Threshold", -80, 0, -52, "dB"), F("range_db", "Range", -80, 0, -30, "dB"),
	     F("ratio", "Ratio", 1, 100, 4, ":1", true), F("attack_ms", "Attack", 0.1, 100, 1, "ms", true),
	     F("hold_ms", "Hold", 0, 500, 40, "ms"), F("release_ms", "Release", 5, 2000, 180, "ms", true),
	     F("hysteresis_db", "Hysteresis", 0, 12, 4, "dB"),
	     F("det_hpf_hz", "Detector high-pass", 20, 2000, 20, "Hz", true),
	     F("det_lpf_hz", "Detector low-pass", 1000, 20000, 20000, "Hz", true), S("sidechain", "Sidechain")});

	add("lvl", "Leveler",
	    {F("target_db", "Target", -36, -6, -18, "dB"), F("max_change_db", "Max change", 0, 24, 12, "dB"),
	     F("speed", "Speed", 0, 100, 40, "%")});

	{
		// Defaults for the first five bands are the "Stream vocal" curve.
		struct D {
			int type;
			double f, g, q;
			int slope;
		};
		const D d[5] = {{EQT_LOWCUT, 80, 0, 0.7071, 3},
				{EQT_LOWSHELF, 140, -1.5, 0.7071, 1},
				{EQT_BELL, 320, -3, 1.2, 1},
				{EQT_BELL, 3200, 3, 0.9, 1},
				{EQT_HIGHSHELF, 12000, 2, 0.7071, 1}};
		std::vector<ParamDef> ps;
		ps.push_back(I("nbands", "Bands", 0, kEqBands, 5, true));
		ps.push_back(C("phase", "Phase", {"Zero latency", "Linear phase"}));
		for (int i = 0; i < kEqBands; i++) {
			const D b = i < 5 ? d[i] : D{EQT_BELL, 1000, 0, 1.0, 1};
			const std::string k = "b" + std::to_string(i) + ".";
			const std::string l = "Band " + std::to_string(i + 1) + " · ";
			ps.push_back(pre(k, l, B("on", "On", true)));
			ps.push_back(pre(k, l,
					 C("type", "Type",
					   {"Bell", "Low shelf", "High shelf", "Low cut", "High cut", "Notch",
					    "Band pass", "Tilt shelf", "Flat tilt"},
					   b.type)));
			ps.push_back(pre(k, l, F("freq_hz", "Frequency", 10, 22000, b.f, "Hz", true)));
			ps.push_back(pre(k, l, F("gain_db", "Gain", -30, 30, b.g, "dB")));
			ps.push_back(pre(k, l, F("q", "Q", 0.025, 40, b.q, "", true)));
			ps.push_back(pre(k, l,
					 C("slope", "Slope",
					   {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct",
					    "72 dB/oct", "96 dB/oct"},
					   b.slope)));
			ps.push_back(
				pre(k, l, C("placement", "Placement", {"Stereo", "Left", "Right", "Mid", "Side"})));
			ps.push_back(pre(k, l, B("solo", "Solo", false)));
			ps.push_back(pre(k, l, B("dyn_on", "Dynamic", false)));
			ps.push_back(pre(k, l, F("dyn_threshold_db", "Dynamic threshold", -60, 0, -30, "dB")));
			ps.push_back(pre(k, l, F("dyn_range_db", "Dynamic range", -24, 24, -6, "dB")));
			ps.push_back(pre(k, l, F("dyn_attack_ms", "Dynamic attack", 0.1, 500, 5, "ms", true)));
			ps.push_back(pre(k, l, F("dyn_release_ms", "Dynamic release", 1, 5000, 80, "ms", true)));
			ps.push_back(pre(k, l, S("dyn_sidechain", "Dynamic sidechain")));
		}
		add("eq", "EQ", std::move(ps));
	}

	add("deess", "De-esser",
	    {F("freq_hz", "Frequency", 2000, 16000, 6500, "Hz", true),
	     F("threshold_db", "Threshold", -60, 0, -28, "dB"), F("range_db", "Range", -24, 0, -8, "dB"),
	     C("mode", "Mode", {"Split band", "Wideband"}), B("listen", "Listen", false)});

	add("pitch", "Pitch correction",
	    {C("key", "Key", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}),
	     C("scale", "Scale", {"Chromatic", "Major", "Minor", "Major pentatonic", "Minor pentatonic"}),
	     F("speed_ms", "Retune speed", 0, 400, 40, "ms"), F("amount", "Amount", 0, 100, 100, "%")});

	add("comp", "Compressor",
	    {F("threshold_db", "Threshold", -60, 0, -20, "dB"), F("ratio", "Ratio", 1, 20, 3, ":1", true),
	     F("knee_db", "Knee", 0, 24, 6, "dB"), F("attack_ms", "Attack", 0.1, 200, 8, "ms", true),
	     F("release_ms", "Release", 5, 2000, 120, "ms", true), B("auto_release", "Auto release", false),
	     F("makeup_db", "Makeup", -12, 24, 4, "dB"), B("auto_makeup", "Auto makeup", false),
	     F("mix", "Mix", 0, 100, 100, "%"), C("detector", "Detector", {"Peak", "RMS"}, 1),
	     F("det_hpf_hz", "Detector high-pass", 20, 500, 20, "Hz", true),
	     C("character", "Character", {"Clean", "Opto"}), S("sidechain", "Sidechain")});

	{
		std::vector<ParamDef> ps = {F("x1_hz", "Low / low-mid crossover", 30, 1000, 180, "Hz", true),
					    F("x2_hz", "Low-mid / high-mid crossover", 200, 8000, 1800, "Hz", true),
					    F("x3_hz", "High-mid / high crossover", 1000, 16000, 6000, "Hz", true)};
		const char *names[kMbBands] = {"Low", "Low-mid", "High-mid", "High"};
		const double th[kMbBands] = {-24, -22, -22, -26}, ratio[kMbBands] = {2, 2.5, 2.5, 2};
		const double at[kMbBands] = {20, 10, 5, 3}, rl[kMbBands] = {200, 150, 100, 80};
		for (int i = 0; i < kMbBands; i++) {
			const std::string k = "b" + std::to_string(i) + ".";
			const std::string l = std::string(names[i]) + " · ";
			ps.push_back(pre(k, l, F("threshold_db", "Threshold", -60, 0, th[i], "dB")));
			ps.push_back(pre(k, l, F("ratio", "Ratio", 1, 20, ratio[i], ":1", true)));
			ps.push_back(pre(k, l, F("attack_ms", "Attack", 0.1, 200, at[i], "ms", true)));
			ps.push_back(pre(k, l, F("release_ms", "Release", 5, 2000, rl[i], "ms", true)));
			ps.push_back(pre(k, l, F("makeup_db", "Makeup", -12, 24, 0, "dB")));
			ps.push_back(pre(k, l, B("solo", "Solo", false)));
		}
		ps.push_back(S("sidechain", "Sidechain"));
		add("mb", "Multiband", std::move(ps));
	}

	add("sat", "Saturation",
	    {F("drive_db", "Drive", 0, 24, 6, "dB"), C("curve", "Curve", {"Tape", "Tube", "Soft clip"}),
	     F("tone", "Tone", -100, 100, 0, "%"), F("mix", "Mix", 0, 100, 100, "%")});

	add("verb", "Reverb",
	    {C("space", "Space", {"Room", "Plate", "Hall"}), F("size", "Size", 0, 100, 45, "%"),
	     F("decay_s", "Decay", 0.2, 10, 1.4, "s", true), F("predelay_ms", "Pre-delay", 0, 200, 20, "ms"),
	     F("damping", "Damping", 0, 100, 50, "%"), F("low_cut_hz", "Low cut", 20, 1000, 200, "Hz", true),
	     F("high_cut_hz", "High cut", 1000, 20000, 8000, "Hz", true), F("mix", "Mix", 0, 100, 15, "%")});

	add("delay", "Delay",
	    {F("time_ms", "Time", 1, 2000, 280, "ms", true), F("feedback", "Feedback", 0, 95, 25, "%"),
	     B("ping_pong", "Ping-pong", false), F("low_cut_hz", "Low cut", 20, 2000, 300, "Hz", true),
	     F("high_cut_hz", "High cut", 1000, 20000, 6000, "Hz", true), F("mix", "Mix", 0, 100, 12, "%")});

	add("lim", "Limiter",
	    {F("ceiling_db", "Ceiling", -12, 0, -1, "dB"), F("release_ms", "Release", 1, 1000, 50, "ms", true),
	     B("true_peak", "True peak", false)});

	add("out", "Output",
	    {F("gain_db", "Gain", -24, 24, 0, "dB"), F("width", "Width", 0, 200, 100, "%"),
	     F("balance", "Balance", -100, 100, 0, "%"), B("mono", "Mono", false),
	     F("chain_mix", "Chain mix", 0, 100, 100, "%"),
	     C("lufs_target", "Loudness target", {"-14 LUFS", "-16 LUFS", "-23 LUFS"}),
	     B("auto_gain", "Auto gain", false)})
		.last = true;

	return r;
}

} // namespace

int ModuleType::index(const std::string &key) const
{
	for (size_t i = 0; i < params.size(); i++)
		if (params[i].key == key)
			return (int)i;
	return -1;
}

const std::vector<ModuleType> &registry()
{
	static const std::vector<ModuleType> r = build();
	return r;
}

const ModuleType *findType(const std::string &type)
{
	for (const ModuleType &m : registry())
		if (m.type == type)
			return &m;
	return nullptr;
}

} // namespace px
