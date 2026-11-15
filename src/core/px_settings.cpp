#include "px_settings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace px {

// ------------------------------------------------------------------ flat JSON
namespace {

struct Parser {
	const std::string &t;
	size_t i = 0;
	std::string err;
	explicit Parser(const std::string &text) : t(text) {}
	void ws()
	{
		while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t'))
			i++;
	}
	bool fail(const char *m)
	{
		if (err.empty())
			err = std::string(m) + " at offset " + std::to_string(i);
		return false;
	}
	bool string(std::string &out)
	{
		if (i >= t.size() || t[i] != '"')
			return fail("expected string");
		i++;
		out.clear();
		while (i < t.size() && t[i] != '"') {
			char c = t[i++];
			if (c != '\\') {
				out += c;
				continue;
			}
			if (i >= t.size())
				return fail("bad escape");
			c = t[i++];
			switch (c) {
			case 'n':
				out += '\n';
				break;
			case 't':
				out += '\t';
				break;
			case 'r':
				out += '\r';
				break;
			case 'b':
				out += '\b';
				break;
			case 'f':
				out += '\f';
				break;
			case 'u': {
				if (i + 4 > t.size())
					return fail("bad \\u escape");
				const unsigned cp = (unsigned)std::strtoul(t.substr(i, 4).c_str(), nullptr, 16);
				i += 4;
				if (cp < 0x80) {
					out += (char)cp;
				} else if (cp < 0x800) {
					out += (char)(0xC0 | (cp >> 6));
					out += (char)(0x80 | (cp & 0x3F));
				} else {
					out += (char)(0xE0 | (cp >> 12));
					out += (char)(0x80 | ((cp >> 6) & 0x3F));
					out += (char)(0x80 | (cp & 0x3F));
				}
				break;
			}
			default:
				out += c; // \" \\ \/
			}
		}
		if (i >= t.size())
			return fail("unterminated string");
		i++;
		return true;
	}
	bool value(Val &v)
	{
		ws();
		if (i >= t.size())
			return fail("expected value");
		if (t[i] == '"') {
			v.type = Val::Str;
			return string(v.s);
		}
		if (t.compare(i, 4, "true") == 0) {
			v = Val::boolean(true);
			i += 4;
			return true;
		}
		if (t.compare(i, 5, "false") == 0) {
			v = Val::boolean(false);
			i += 5;
			return true;
		}
		if (t.compare(i, 4, "null") == 0) {
			v = Val::num(0);
			i += 4;
			return true;
		}
		char *end = nullptr;
		const double d = std::strtod(t.c_str() + i, &end);
		if (end == t.c_str() + i)
			return fail("expected a string, number or boolean (presets are flat objects)");
		i = (size_t)(end - t.c_str());
		v = Val::num(d);
		return true;
	}
};

std::string quote(const std::string &s)
{
	std::string o = "\"";
	for (char c : s) {
		if (c == '"' || c == '\\') {
			o += '\\';
			o += c;
		} else if (c == '\n') {
			o += "\\n";
		} else if ((unsigned char)c < 0x20) {
			char b[8];
			std::snprintf(b, sizeof b, "\\u%04x", c);
			o += b;
		} else {
			o += c;
		}
	}
	return o + "\"";
}

std::vector<std::string> split(const std::string &s, char sep)
{
	std::vector<std::string> out;
	std::string cur;
	for (char c : s) {
		if (c == sep) {
			if (!cur.empty())
				out.push_back(cur);
			cur.clear();
		} else if (c != ' ') {
			cur += c;
		}
	}
	if (!cur.empty())
		out.push_back(cur);
	return out;
}

std::string join(const std::vector<std::string> &v)
{
	std::string s;
	for (size_t i = 0; i < v.size(); i++)
		s += (i ? "," : "") + v[i];
	return s;
}

std::string typeOf(const Store &s, const std::string &id)
{
	std::string t;
	s.getStr(id + ".type", t);
	return t;
}

void setChain(Store &s, const std::vector<std::string> &ids)
{
	s.set("chain", Val::str(join(ids)));
}

void eraseModuleKeys(Store &s, const std::string &id)
{
	if (const ModuleType *mt = findType(typeOf(s, id)))
		for (const ParamDef &p : mt->params)
			s.erase(id + "." + p.key);
	s.erase(id + ".type");
	s.erase(id + ".on");
}

bool locked(const Store &s, const std::string &id)
{
	const ModuleType *mt = findType(typeOf(s, id));
	return !mt || mt->first || mt->last;
}

} // namespace

bool parseFlatJson(const std::string &text, FlatMap &out, std::string *error)
{
	Parser p(text);
	auto bad = [&](const char *m) {
		p.fail(m);
		if (error)
			*error = p.err;
		return false;
	};
	p.ws();
	if (p.i >= text.size() || text[p.i] != '{')
		return bad("expected '{'");
	p.i++;
	p.ws();
	if (p.i < text.size() && text[p.i] == '}')
		return true;
	for (;;) {
		p.ws();
		std::string key;
		Val v;
		if (!p.string(key))
			return bad("expected key");
		p.ws();
		if (p.i >= text.size() || text[p.i] != ':')
			return bad("expected ':'");
		p.i++;
		if (!p.value(v))
			return bad("bad value");
		out[key] = v;
		p.ws();
		if (p.i < text.size() && text[p.i] == ',') {
			p.i++;
			continue;
		}
		if (p.i < text.size() && text[p.i] == '}')
			return true;
		return bad("expected ',' or '}'");
	}
}

std::string toFlatJson(const FlatMap &map)
{
	std::string o = "{\n";
	size_t n = 0;
	for (const auto &kv : map) {
		o += "  " + quote(kv.first) + ": ";
		if (kv.second.type == Val::Str) {
			o += quote(kv.second.s);
		} else if (kv.second.type == Val::Bool) {
			o += kv.second.n != 0 ? "true" : "false";
		} else {
			char b[40];
			std::snprintf(b, sizeof b, "%.10g", kv.second.n);
			o += b;
		}
		o += ++n < map.size() ? ",\n" : "\n";
	}
	return o + "}\n";
}

bool MapStore::getNum(const std::string &key, double &out) const
{
	auto it = map.find(key);
	if (it == map.end() || it->second.type == Val::Str)
		return false;
	out = it->second.n;
	return true;
}
bool MapStore::getStr(const std::string &key, std::string &out) const
{
	auto it = map.find(key);
	if (it == map.end() || it->second.type != Val::Str)
		return false;
	out = it->second.s;
	return true;
}

// ------------------------------------------------------------------ rack
std::vector<std::string> chainIds(const Store &s)
{
	std::string c;
	s.getStr("chain", c);
	return split(c, ',');
}

ChainCfg readChain(const Store &s)
{
	ChainCfg cfg;
	for (const std::string &id : chainIds(s)) {
		const ModuleType *mt = findType(typeOf(s, id));
		if (!mt || cfg.modules.size() >= (size_t)kMaxModules)
			continue; // unknown module types are skipped, not deleted
		ModuleCfg m;
		m.id = id;
		m.type = mt->type;
		double on = 1;
		s.getNum(id + ".on", on);
		m.on = on != 0;
		m.values.reserve(mt->params.size());
		for (const ParamDef &p : mt->params) {
			const std::string key = id + "." + p.key;
			if (p.kind == Kind::Source) {
				std::string name;
				int slot = -1;
				if (s.getStr(key, name) && !name.empty()) {
					auto it = std::find(cfg.sidechains.begin(), cfg.sidechains.end(), name);
					if (it != cfg.sidechains.end()) {
						slot = (int)(it - cfg.sidechains.begin());
					} else if (cfg.sidechains.size() < 8) {
						slot = (int)cfg.sidechains.size();
						cfg.sidechains.push_back(name);
					}
				}
				m.values.push_back(slot);
				continue;
			}
			double v = p.def;
			s.getNum(key, v);
			m.values.push_back(std::min(std::max(v, p.min), p.max));
		}
		cfg.modules.push_back(std::move(m));
	}
	return cfg;
}

void ensureChain(Store &s)
{
	if (chainIds(s).empty())
		applyChain(s, defaultChain());
}

std::string addModule(Store &s, const std::string &type)
{
	const ModuleType *mt = findType(type);
	std::vector<std::string> ids = chainIds(s);
	if (!mt || mt->first || mt->last || ids.size() >= (size_t)kMaxModules || ids.empty())
		return "";
	int next = 1;
	for (const std::string &id : ids)
		if (id.size() > 1 && id[0] == 'm')
			next = std::max(next, std::atoi(id.c_str() + 1) + 1);
	const std::string id = "m" + std::to_string(next);
	// Before Output, and before a trailing limiter so the limiter stays last.
	size_t at = ids.size() - 1;
	if (type != "lim" && at >= 1 && typeOf(s, ids[at - 1]) == "lim")
		at--;
	ids.insert(ids.begin() + (long long)at, id);
	s.set(id + ".type", Val::str(type));
	s.set(id + ".on", Val::boolean(true));
	setChain(s, ids);
	return id;
}

bool removeModule(Store &s, const std::string &id)
{
	std::vector<std::string> ids = chainIds(s);
	auto it = std::find(ids.begin(), ids.end(), id);
	if (it == ids.end() || locked(s, id))
		return false;
	eraseModuleKeys(s, id);
	ids.erase(it);
	setChain(s, ids);
	return true;
}

bool moveModule(Store &s, const std::string &id, int direction)
{
	std::vector<std::string> ids = chainIds(s);
	auto it = std::find(ids.begin(), ids.end(), id);
	if (it == ids.end() || locked(s, id))
		return false;
	const long long i = it - ids.begin(), j = i + (direction < 0 ? -1 : 1);
	if (j < 1 || j > (long long)ids.size() - 2)
		return false;
	std::swap(ids[(size_t)i], ids[(size_t)j]);
	setChain(s, ids);
	return true;
}

bool addEqBand(Store &s, const std::string &id)
{
	const ModuleType *mt = findType(typeOf(s, id));
	if (!mt || mt->type != "eq")
		return false;
	double n = mt->params[0].def;
	s.getNum(id + ".nbands", n);
	if (n >= kEqBands)
		return false;
	// the new slot starts from its defaults, then becomes a flat bell at 1 kHz
	const std::string k = id + ".b" + std::to_string((int)n) + ".";
	for (int p = 0; p < kEqBandParams; p++)
		s.erase(id + "." + mt->params[(size_t)(kEqFirstBand + (int)n * kEqBandParams + p)].key);
	s.set(k + "type", Val::num(EQT_BELL));
	s.set(k + "freq_hz", Val::num(1000));
	s.set(k + "gain_db", Val::num(0));
	s.set(k + "q", Val::num(1));
	s.set(id + ".nbands", Val::num(n + 1));
	return true;
}

bool removeEqBand(Store &s, const std::string &id, int band)
{
	const ModuleType *mt = findType(typeOf(s, id));
	if (!mt || mt->type != "eq")
		return false;
	double n = mt->params[0].def;
	s.getNum(id + ".nbands", n);
	if (band < 0 || band >= (int)n)
		return false;
	auto key = [&](int b, int p) {
		return id + "." + mt->params[(size_t)(kEqFirstBand + b * kEqBandParams + p)].key;
	};
	for (int b = band; b < (int)n; b++)
		for (int p = 0; p < kEqBandParams; p++) {
			const ParamDef &def = mt->params[(size_t)(kEqFirstBand + b * kEqBandParams + p)];
			if (b == (int)n - 1) {
				s.erase(key(b, p));
				continue;
			}
			// Shift the band above down. Defaults differ per slot, so write the value explicitly.
			const ParamDef &src = mt->params[(size_t)(kEqFirstBand + (b + 1) * kEqBandParams + p)];
			std::string str;
			double num = src.def;
			if (def.kind == Kind::Source) {
				if (s.getStr(key(b + 1, p), str))
					s.set(key(b, p), Val::str(str));
				else
					s.erase(key(b, p));
			} else {
				s.getNum(key(b + 1, p), num);
				s.set(key(b, p), def.kind == Kind::Bool ? Val::boolean(num != 0) : Val::num(num));
			}
		}
	s.set(id + ".nbands", Val::num(n - 1));
	return true;
}

bool limiterMisplaced(const Store &s)
{
	const std::vector<std::string> ids = chainIds(s);
	for (size_t i = 0; i < ids.size(); i++)
		if (typeOf(s, ids[i]) == "lim") {
			double on = 1;
			s.getNum(ids[i] + ".on", on);
			if (on != 0 && i + 2 != ids.size())
				return true;
		}
	return false;
}

FlatMap captureChain(const Store &s)
{
	FlatMap out;
	const std::vector<std::string> ids = chainIds(s);
	out["chain"] = Val::str(join(ids));
	for (const std::string &id : ids) {
		const ModuleType *mt = findType(typeOf(s, id));
		if (!mt)
			continue;
		out[id + ".type"] = Val::str(mt->type);
		double on = 1;
		s.getNum(id + ".on", on);
		out[id + ".on"] = Val::boolean(on != 0);
		int nbands = kEqBands;
		if (mt->type == "eq") {
			double n = mt->params[0].def;
			s.getNum(id + ".nbands", n);
			nbands = (int)n;
		}
		for (size_t i = 0; i < mt->params.size(); i++) {
			const ParamDef &p = mt->params[i];
			// unused EQ slots and one-shot tokens don't belong in a preset
			if (mt->type == "eq" && (int)i >= kEqFirstBand + nbands * kEqBandParams)
				break;
			if (p.key == "learn")
				continue;
			const std::string key = id + "." + p.key;
			std::string str;
			double num = p.def;
			if (p.kind == Kind::Source) {
				if (s.getStr(key, str) && !str.empty())
					out[key] = Val::str(str);
			} else {
				s.getNum(key, num);
				out[key] = p.kind == Kind::Bool ? Val::boolean(num != 0) : Val::num(num);
			}
		}
	}
	return out;
}

void applyChain(Store &s, const FlatMap &preset)
{
	for (const std::string &id : chainIds(s))
		eraseModuleKeys(s, id);
	for (const auto &kv : preset) {
		const std::string &key = kv.first;
		if (key.empty() || key[0] == '_')
			continue;
		Val v = kv.second;
		// Coerce to the parameter's kind, so a preset written with 0/1 still drives a checkbox.
		const size_t dot = key.find('.');
		if (dot != std::string::npos && v.type != Val::Str) {
			const std::string id = key.substr(0, dot), rest = key.substr(dot + 1);
			auto t = preset.find(id + ".type");
			const ModuleType *mt = t == preset.end() ? nullptr : findType(t->second.s);
			const int idx = mt ? mt->index(rest) : -1;
			const bool isBool = rest == "on" || (idx >= 0 && mt->params[(size_t)idx].kind == Kind::Bool);
			v = isBool ? Val::boolean(v.n != 0) : Val::num(v.n);
		}
		s.set(key, v);
	}
}

FlatMap captureModule(const Store &s, const std::string &id)
{
	FlatMap out;
	const ModuleType *mt = findType(typeOf(s, id));
	if (!mt)
		return out;
	const std::string prefix = id + ".";
	for (const auto &kv : captureChain(s))
		if (kv.first.compare(0, prefix.size(), prefix) == 0) {
			const std::string key = kv.first.substr(prefix.size());
			if (key != "type" && key != "on")
				out[key] = kv.second;
		}
	out["_type"] = Val::str(mt->type);
	return out;
}

bool applyModule(Store &s, const std::string &id, const FlatMap &preset)
{
	const ModuleType *mt = findType(typeOf(s, id));
	const auto t = preset.find("_type");
	if (!mt || t == preset.end() || t->second.s != mt->type)
		return false;
	for (const ParamDef &p : mt->params)
		s.erase(id + "." + p.key);
	for (const auto &kv : preset) {
		const int idx = mt->index(kv.first);
		if (idx < 0)
			continue; // "_name", "_type", and keys from a newer version
		const ParamDef &p = mt->params[(size_t)idx];
		if (kv.second.type == Val::Str)
			s.set(id + "." + kv.first, kv.second);
		else
			s.set(id + "." + kv.first,
			      p.kind == Kind::Bool ? Val::boolean(kv.second.n != 0) : Val::num(kv.second.n));
	}
	return true;
}

const FlatMap &defaultChain()
{
	static const FlatMap m = [] {
		FlatMap d;
		const char *types[] = {"trim", "gate", "eq", "deess", "comp", "lim", "out"};
		std::string chain;
		for (int i = 0; i < 7; i++) {
			const std::string id = "m" + std::to_string(i + 1);
			chain += (i ? "," : "") + id;
			d[id + ".type"] = Val::str(types[i]);
			d[id + ".on"] = Val::boolean(true);
		}
		d["chain"] = Val::str(chain);
		d["m5.character"] = Val::num(1); // Opto
		return d;
	}();
	return m;
}

} // namespace px
