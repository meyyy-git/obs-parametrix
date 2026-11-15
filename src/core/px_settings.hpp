// Settings model. Settings are a flat key/value set so OBS properties and
// obs-websocket can address every parameter directly:
//
//   "chain"            : "m1,m2,m3"        rack order (module ids)
//   "<id>.type"        : "comp"
//   "<id>.on"          : true
//   "<id>.<param key>" : value             e.g. "m3.b0.freq_hz": 80
//
// A preset file is the same flat object as JSON, plus optional "_name" and "_category".
#pragma once

#include "px_params.hpp"

#include <map>
#include <string>
#include <vector>

namespace px {

struct Val {
	enum Type { Num, Bool, Str } type = Num;
	double n = 0;
	std::string s;
	static Val num(double v)
	{
		Val x;
		x.n = v;
		return x;
	}
	static Val boolean(bool v)
	{
		Val x;
		x.type = Bool;
		x.n = v ? 1 : 0;
		return x;
	}
	static Val str(std::string v)
	{
		Val x;
		x.type = Str;
		x.s = std::move(v);
		return x;
	}
};
using FlatMap = std::map<std::string, Val>;

bool parseFlatJson(const std::string &text, FlatMap &out, std::string *error = nullptr);
std::string toFlatJson(const FlatMap &map);

// Abstract key/value store: obs_data_t in the plugin, FlatMap in tests and tools.
class Store {
public:
	virtual ~Store() = default;
	virtual bool getNum(const std::string &key, double &out) const = 0; // numbers and bools
	virtual bool getStr(const std::string &key, std::string &out) const = 0;
	virtual void set(const std::string &key, const Val &v) = 0;
	virtual void erase(const std::string &key) = 0;
};

class MapStore : public Store {
public:
	FlatMap map;
	bool getNum(const std::string &key, double &out) const override;
	bool getStr(const std::string &key, std::string &out) const override;
	void set(const std::string &key, const Val &v) override { map[key] = v; }
	void erase(const std::string &key) override { map.erase(key); }
};

struct ModuleCfg {
	std::string id, type;
	bool on = true;
	std::vector<double> values; // ParamDef order; Source params hold a sidechain slot or -1
};
struct ChainCfg {
	std::vector<ModuleCfg> modules;
	std::vector<std::string> sidechains; // slot -> OBS source name
};

constexpr int kMaxModules = 16;

std::vector<std::string> chainIds(const Store &s);
// Reads the rack. Missing parameters take their registry default.
ChainCfg readChain(const Store &s);

// Rack edits. Each keeps Trim first and Output last.
void ensureChain(Store &s);                               // writes the default rack if there is none
std::string addModule(Store &s, const std::string &type); // returns the new id, "" if refused
bool removeModule(Store &s, const std::string &id);
bool moveModule(Store &s, const std::string &id, int direction); // -1 earlier, +1 later
bool addEqBand(Store &s, const std::string &id);
bool removeEqBand(Store &s, const std::string &id, int band);
// True when a limiter exists but is not the last module before Output.
bool limiterMisplaced(const Store &s);

// Presets: the rack keys of a store as a flat map, and back.
FlatMap captureChain(const Store &s);
void applyChain(Store &s, const FlatMap &preset);
const FlatMap &defaultChain(); // "Stream vocal"

// Module presets hold one module's parameters, keyed without the module id, plus "_type".
FlatMap captureModule(const Store &s, const std::string &id);
// Replaces the module's parameters. False if the preset is for another module type.
bool applyModule(Store &s, const std::string &id, const FlatMap &preset);

} // namespace px
