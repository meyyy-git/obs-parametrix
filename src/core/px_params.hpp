// Parameter registry: every module parameter is declared once here.
// The registry drives the settings keys, OBS properties, presets and defaults.
#pragma once

#include <string>
#include <vector>

namespace px {

enum class Kind { Float, Int, Bool, Choice, Source };

struct ParamDef {
	std::string key;   // settings key suffix, e.g. "threshold_db" or "b3.freq_hz"; never "type" or "on"
	std::string label; // English label; the plugin looks up a translation first
	double min = 0, max = 1, def = 0;
	std::string unit; // "dB", "Hz", "ms", "s", "%", ":1" or ""
	bool log = false;
	Kind kind = Kind::Float;
	std::vector<std::string> options; // Kind::Choice
	bool hidden = false;              // stored, but not shown as a control
};

struct ModuleType {
	std::string type;                 // "comp"
	std::string name;                 // "Compressor"
	bool first = false, last = false; // locked position in the rack
	std::vector<ParamDef> params;
	int index(const std::string &key) const;
};

const std::vector<ModuleType> &registry();
const ModuleType *findType(const std::string &type);

// EQ layout shared by the module and the UI
constexpr int kEqBands = 24;
constexpr int kEqBandParams = 14;
constexpr int kEqFirstBand = 2; // index 0 is nbands, 1 is the phase mode
enum EqBandParam {
	EQ_ON,
	EQ_TYPE,
	EQ_FREQ,
	EQ_GAIN,
	EQ_Q,
	EQ_SLOPE,
	EQ_PLACE,
	EQ_SOLO,
	EQ_DYN_ON,
	EQ_DYN_TH,
	EQ_DYN_RANGE,
	EQ_DYN_ATK,
	EQ_DYN_REL,
	EQ_DYN_SC
};
enum EqType {
	EQT_BELL,
	EQT_LOWSHELF,
	EQT_HIGHSHELF,
	EQT_LOWCUT,
	EQT_HIGHCUT,
	EQT_NOTCH,
	EQT_BANDPASS,
	EQT_TILT,
	EQT_FLATTILT
};
extern const int kEqSlopes[8]; // dB/oct per slope choice
// Magnitude in dB of one EQ band at `freq`, from the same filter design the audio uses.
// `p` points at the band's kEqBandParams values; `gainDb` replaces p[EQ_GAIN].
double eqBandDb(const double *p, double gainDb, double sr, double freq);

// Multiband layout
constexpr int kMbBands = 4;
constexpr int kMbBandParams = 6;
constexpr int kMbFirstBand = 3; // 0..2 are the crossovers

} // namespace px
