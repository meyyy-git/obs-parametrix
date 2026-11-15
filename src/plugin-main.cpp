/*
Parametrix - all-in-one vocal channel strip for OBS Studio
Copyright (C) 2026 Parametrix contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include "px_chain.hpp"

#ifdef PX_ENABLE_EDITOR
#include "editor/editor.hpp"
#include <obs-frontend-api.h>
#include <QPointer>
#include <QWidget>
#endif

#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr int kSlots = 8;

// Translation with an English fallback, so untranslated registry labels still show.
const char *tr(const std::string &key, const char *fallback)
{
	const char *out = nullptr;
	return obs_module_get_string(key.c_str(), &out) && out ? out : fallback;
}
#define T(key) obs_module_text(key)

// ------------------------------------------------------------------ settings bridge
class ObsStore : public px::Store {
public:
	explicit ObsStore(obs_data_t *data) : d(data) {}
	bool getNum(const std::string &key, double &out) const override
	{
		obs_data_item_t *it = obs_data_item_byname(d, key.c_str());
		if (!it)
			return false;
		bool ok = true;
		switch (obs_data_item_gettype(it)) {
		case OBS_DATA_NUMBER:
			out = obs_data_item_get_double(it);
			break;
		case OBS_DATA_BOOLEAN:
			out = obs_data_item_get_bool(it) ? 1.0 : 0.0;
			break;
		default:
			ok = false;
		}
		obs_data_item_release(&it);
		return ok;
	}
	bool getStr(const std::string &key, std::string &out) const override
	{
		obs_data_item_t *it = obs_data_item_byname(d, key.c_str());
		if (!it)
			return false;
		const bool ok = obs_data_item_gettype(it) == OBS_DATA_STRING;
		if (ok)
			out = obs_data_item_get_string(it);
		obs_data_item_release(&it);
		return ok;
	}
	void set(const std::string &key, const px::Val &v) override
	{
		if (v.type == px::Val::Str)
			obs_data_set_string(d, key.c_str(), v.s.c_str());
		else if (v.type == px::Val::Bool)
			obs_data_set_bool(d, key.c_str(), v.n != 0);
		else
			obs_data_set_double(d, key.c_str(), v.n);
	}
	void erase(const std::string &key) override { obs_data_erase(d, key.c_str()); }

private:
	obs_data_t *d;
};

// ------------------------------------------------------------------ presets on disk
struct PresetInfo {
	std::string path, name, category;
	bool user = false;
};

// Chain presets live in "presets", single-module presets in "module-presets".
std::string moduleDir(bool user, const char *sub = "presets")
{
	char *p = user ? obs_module_config_path(sub) : obs_module_file(sub);
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

bool readPreset(const std::string &path, px::FlatMap &out)
{
	char *text = os_quick_read_utf8_file(path.c_str());
	if (!text)
		return false;
	std::string err;
	const bool ok = px::parseFlatJson(text, out, &err);
	bfree(text);
	if (!ok)
		obs_log(LOG_WARNING, "preset %s: %s", path.c_str(), err.c_str());
	return ok;
}

// With `type` set, lists the module presets of that module type instead of chain presets.
std::vector<PresetInfo> listPresets(const std::string &type = "")
{
	std::vector<PresetInfo> out;
	for (int user = 0; user < 2; user++) {
		const std::string dir = moduleDir(user != 0, type.empty() ? "presets" : "module-presets");
		os_dir_t *d = dir.empty() ? nullptr : os_opendir(dir.c_str());
		if (!d)
			continue;
		std::vector<std::string> files;
		while (os_dirent *e = os_readdir(d)) {
			const std::string n = e->d_name;
			if (!e->directory && n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0)
				files.push_back(n);
		}
		os_closedir(d);
		std::sort(files.begin(), files.end());
		for (const std::string &f : files) {
			px::FlatMap m;
			PresetInfo p;
			p.path = dir + "/" + f;
			p.user = user != 0;
			if (!readPreset(p.path, m))
				continue;
			if (!type.empty() && (!m.count("_type") || m["_type"].s != type))
				continue;
			p.name = m.count("_name") ? m["_name"].s : f.substr(0, f.size() - 5);
			p.category = p.user ? T("Px.Category.User")
					    : (m.count("_category") ? m["_category"].s
								    : std::string(T("Px.Category.Factory")));
			out.push_back(std::move(p));
		}
	}
	return out;
}

std::string safeFileName(std::string s)
{
	for (char &c : s)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
		      c == '_' || c == ' '))
			c = '_';
	while (!s.empty() && s.back() == ' ')
		s.pop_back();
	return s.empty() ? "preset" : s;
}

bool writePreset(const std::string &path, obs_data_t *settings, const std::string &name)
{
	ObsStore st(settings);
	px::FlatMap m = px::captureChain(st);
	m["_name"] = px::Val::str(name);
	m["_category"] = px::Val::str("User");
	const std::string json = px::toFlatJson(m);
	return os_quick_write_utf8_file(path.c_str(), json.c_str(), json.size(), false);
}

// ------------------------------------------------------------------ sidechain capture
struct ScSource {
	std::string name;
	obs_weak_source_t *weak = nullptr;
	std::mutex m;
	std::deque<float> q[px::kMaxCh];
	std::vector<float> buf[px::kMaxCh];
	size_t limit = 48000;
	int ch = 2;
};

void scCapture(void *param, obs_source_t *, const struct audio_data *audio, bool muted)
{
	ScSource *s = static_cast<ScSource *>(param);
	std::lock_guard<std::mutex> lock(s->m);
	for (int c = 0; c < s->ch; c++) {
		const float *in = (const float *)audio->data[c];
		std::deque<float> &q = s->q[c];
		if (muted || !in)
			q.insert(q.end(), audio->frames, 0.0f);
		else
			q.insert(q.end(), in, in + audio->frames);
		if (q.size() > s->limit) // nobody is reading: keep only the newest second
			q.erase(q.begin(), q.begin() + (long long)(q.size() - s->limit));
	}
}

void scDetach(ScSource *s)
{
	if (!s->weak)
		return;
	if (obs_source_t *src = obs_weak_source_get_source(s->weak)) {
		obs_source_remove_audio_capture_callback(src, scCapture, s);
		obs_source_release(src);
	}
	obs_weak_source_release(s->weak);
	s->weak = nullptr;
}

bool scAttach(ScSource *s)
{
	obs_source_t *src = obs_get_source_by_name(s->name.c_str());
	if (!src)
		return false;
	s->weak = obs_source_get_weak_source(src);
	obs_source_add_audio_capture_callback(src, scCapture, s);
	obs_source_release(src);
	return true;
}

// ------------------------------------------------------------------ the filter
struct Filter;
struct Indexed { // hotkey payload: which preset slot or rack position
	Filter *f;
	int index;
};

struct Filter {
	obs_source_t *source = nullptr;
	// Set for a standalone one-module filter ("Parametrix Compressor"); null for the rack.
	const px::ModuleType *single = nullptr;
	px::Chain chain;
	double sr = 48000;
	int ch = 2;

	std::mutex scMutex;
	std::vector<std::unique_ptr<ScSource>> sc;
	float scRetry = 0;

	std::string current; // name of the preset loaded last, for next/previous
	Indexed hkSlots[kSlots], hkModules[px::kMaxModules];

	std::atomic<float> cpu{0.0f}; // percent of one core, smoothed

	// Capture loop: 0 idle, 1 recording, 2 playing. The buffer is allocated by the UI
	// under scMutex, which the audio thread holds while it runs.
	std::atomic<int> loop{0};
	int loopSeen = 0;
	size_t loopPos = 0, loopLen = 0;
	std::vector<float> loopBuf[px::kMaxCh];

#ifdef PX_ENABLE_EDITOR
	// The editor outlives a destroyed filter by one timer tick; it reaches the filter
	// only through this link, which filterDestroy clears.
	struct Link {
		std::mutex m;
		Filter *f = nullptr;
	};
	std::shared_ptr<Link> link;
	QPointer<QWidget> editor;
#endif
};

void filterUpdate(void *data, obs_data_t *settings)
{
	Filter *f = static_cast<Filter *>(data);
	audio_t *audio = obs_get_audio();
	f->sr = audio_output_get_sample_rate(audio);
	f->ch = (int)std::min<size_t>(audio_output_get_channels(audio), px::kMaxCh);

	ObsStore st(settings);
	if (!f->single) {
		px::ensureChain(st);
	} else if (px::chainIds(st).empty()) { // a standalone filter is a rack of exactly one module
		st.set("chain", px::Val::str("m1"));
		st.set("m1.type", px::Val::str(f->single->type));
		st.set("m1.on", px::Val::boolean(true));
	}
	const px::ChainCfg cfg = px::readChain(st);

	{
		std::lock_guard<std::mutex> lock(f->scMutex);
		bool same = cfg.sidechains.size() == f->sc.size();
		for (size_t i = 0; same && i < f->sc.size(); i++)
			same = f->sc[i]->name == cfg.sidechains[i];
		if (!same) {
			for (auto &s : f->sc)
				scDetach(s.get());
			f->sc.clear();
			for (const std::string &name : cfg.sidechains) {
				auto s = std::make_unique<ScSource>();
				s->name = name;
				s->ch = f->ch;
				s->limit = (size_t)f->sr;
				for (auto &b : s->buf)
					b.assign(4096, 0.0f);
				scAttach(s.get());
				f->sc.push_back(std::move(s));
			}
		}
	}
	f->chain.configure(cfg, f->sr, f->ch);
}

void filterTick(void *data, float seconds)
{
	// A sidechain source may not exist yet while a scene collection loads: keep trying.
	Filter *f = static_cast<Filter *>(data);
	f->scRetry += seconds;
	if (f->scRetry < 3.0f)
		return;
	f->scRetry = 0;
	std::lock_guard<std::mutex> lock(f->scMutex);
	for (auto &s : f->sc)
		if (!s->weak)
			scAttach(s.get());
}

struct obs_audio_data *filterAudio(void *data, struct obs_audio_data *audio)
{
	Filter *f = static_cast<Filter *>(data);
	float *io[px::kMaxCh];
	int ch = 0;
	while (ch < f->ch && audio->data[ch]) {
		io[ch] = (float *)audio->data[ch];
		ch++;
	}
	if (ch == 0 || audio->frames == 0)
		return audio;
	const size_t n = audio->frames;

	px::ScBlock blocks[px::kMaxSidechains];
	int nsc = 0;
	std::lock_guard<std::mutex> lock(f->scMutex);
	for (auto &s : f->sc) {
		if (nsc >= px::kMaxSidechains)
			break;
		px::ScBlock &b = blocks[nsc++];
		std::lock_guard<std::mutex> sl(s->m);
		if (!s->weak || s->q[0].size() < n)
			continue; // no audio from that source right now: the module falls back to its own input
		for (int c = 0; c < s->ch; c++) {
			if (s->buf[c].size() < n)
				s->buf[c].resize(n);
			std::copy(s->q[c].begin(), s->q[c].begin() + (long long)n, s->buf[c].begin());
			s->q[c].erase(s->q[c].begin(), s->q[c].begin() + (long long)n);
			b.ch[c] = s->buf[c].data();
		}
		b.nch = s->ch;
	}
	// capture loop: record the dry input, then play it in place of the live input
	const int loop = f->loop.load();
	if (loop != f->loopSeen) {
		f->loopSeen = loop;
		f->loopPos = 0;
	}
	if (loop && f->loopLen && f->loopBuf[ch - 1].size() >= f->loopLen) {
		for (size_t i = 0; i < n; i++) {
			const size_t pos = (f->loopPos + i) % f->loopLen;
			for (int c = 0; c < ch; c++) {
				if (loop == 1)
					f->loopBuf[c][pos] = io[c][i];
				else
					io[c][i] = f->loopBuf[c][pos];
			}
		}
		f->loopPos += n;
		if (loop == 1 && f->loopPos >= f->loopLen) {
			f->loop.store(2);
			f->loopSeen = 2;
			f->loopPos = 0;
		} else if (loop == 2) {
			f->loopPos %= f->loopLen;
		}
	}

	const uint64_t t0 = os_gettime_ns();
	f->chain.process(io, ch, (int)n, blocks, nsc);
	const double pct = 100.0 * (double)(os_gettime_ns() - t0) / ((double)n / f->sr * 1e9);
	f->cpu.store(f->cpu.load() * 0.95f + (float)pct * 0.05f);
	return audio;
}

// ------------------------------------------------------------------ preset loading
void applyPresetFile(Filter *f, obs_data_t *settings, const PresetInfo &p)
{
	px::FlatMap m;
	if (!readPreset(p.path, m))
		return;
	ObsStore st(settings);
	px::applyChain(st, m);
	f->current = p.name;
	obs_data_set_string(settings, "px_current", p.name.c_str());
}

void loadPresetFromHotkey(Filter *f, const std::string &name, int step)
{
	const std::vector<PresetInfo> all = listPresets();
	if (all.empty())
		return;
	long long idx = -1;
	for (size_t i = 0; i < all.size(); i++)
		if (all[i].name == (step ? f->current : name))
			idx = (long long)i;
	if (step)
		idx = ((idx < 0 ? 0 : idx + step) + (long long)all.size()) % (long long)all.size();
	if (idx < 0)
		return;
	obs_data_t *settings = obs_source_get_settings(f->source);
	applyPresetFile(f, settings, all[(size_t)idx]);
	obs_source_update(f->source, nullptr);
	obs_data_release(settings);
	obs_source_update_properties(f->source);
}

// ------------------------------------------------------------------ properties
bool enumAudioSources(void *param, obs_source_t *src)
{
	if (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO)
		static_cast<std::vector<std::string> *>(param)->push_back(obs_source_get_name(src));
	return true;
}

std::string paramLabel(const px::ModuleType &mt, const px::ParamDef &p)
{
	// "b3.freq_hz" -> translated "Band 4 · Frequency"; the suffix is shared across modules.
	const size_t dot = p.key.rfind('.');
	const std::string suffix = dot == std::string::npos ? p.key : p.key.substr(dot + 1);
	const size_t sep = p.label.rfind(" \xC2\xB7 ");
	const std::string fallback = sep == std::string::npos ? p.label : p.label.substr(sep + 4);
	std::string label = tr("Px.Param." + suffix, fallback.c_str());
	if (dot == std::string::npos)
		return label;
	std::string prefix = p.label.substr(0, sep);
	if (mt.type == "eq")
		prefix = std::string(T("Px.Band")) + " " + std::to_string(std::atoi(p.key.c_str() + 1) + 1);
	return prefix + " \xC2\xB7 " + label;
}

void addParam(obs_properties_t *g, obs_data_t *settings, const std::string &id, const px::ModuleType &mt,
	      const px::ParamDef &p, const std::vector<std::string> &sources)
{
	const std::string key = id + "." + p.key, label = paramLabel(mt, p);
	switch (p.kind) {
	case px::Kind::Float: {
		obs_data_set_default_double(settings, key.c_str(), p.def);
		const double step = p.unit.empty() ? 0.01 : 0.1;
		obs_property_t *prop =
			p.log ? obs_properties_add_float(g, key.c_str(), label.c_str(), p.min, p.max, step)
			      : obs_properties_add_float_slider(g, key.c_str(), label.c_str(), p.min, p.max, step);
		if (!p.unit.empty())
			obs_property_float_set_suffix(prop, (" " + p.unit).c_str());
		break;
	}
	case px::Kind::Bool:
		obs_data_set_default_bool(settings, key.c_str(), p.def != 0);
		obs_properties_add_bool(g, key.c_str(), label.c_str());
		break;
	case px::Kind::Choice: {
		obs_data_set_default_int(settings, key.c_str(), (long long)p.def);
		obs_property_t *prop = obs_properties_add_list(g, key.c_str(), label.c_str(), OBS_COMBO_TYPE_LIST,
							       OBS_COMBO_FORMAT_INT);
		for (size_t i = 0; i < p.options.size(); i++)
			obs_property_list_add_int(prop, tr("Px.Option." + p.options[i], p.options[i].c_str()),
						  (long long)i);
		break;
	}
	case px::Kind::Source: {
		obs_property_t *prop = obs_properties_add_list(g, key.c_str(), label.c_str(), OBS_COMBO_TYPE_LIST,
							       OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(prop, T("Px.Sidechain.Internal"), "");
		for (const std::string &s : sources)
			obs_property_list_add_string(prop, s.c_str(), s.c_str());
		break;
	}
	case px::Kind::Int:
		break; // hidden bookkeeping values
	}
}

void fillPresetList(obs_property_t *list, bool withPrompt)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, withPrompt ? T("Px.Preset.Choose") : T("Px.Slot.None"), "");
	for (const PresetInfo &p : listPresets())
		obs_property_list_add_string(list, (p.category + " / " + p.name).c_str(),
					     withPrompt ? p.path.c_str() : p.name.c_str());
}

bool isControl(const char *name)
{
	return std::strncmp(name, "px_", 3) == 0;
}

// Rebuilds the per-module groups in place. The px_* controls above them are never
// removed, so a callback that triggers a rebuild never frees its own property.
void rebuildModules(obs_properties_t *props, Filter *f, obs_data_t *settings)
{
	std::vector<std::string> old;
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p))
		if (!isControl(obs_property_name(p)))
			old.push_back(obs_property_name(p));
	for (const std::string &n : old)
		obs_properties_remove_by_name(props, n.c_str());

	std::vector<std::string> sources;
	obs_enum_sources(enumAudioSources, &sources);
	if (obs_source_t *parent = obs_filter_get_parent(f->source)) {
		const char *self = obs_source_get_name(parent);
		sources.erase(std::remove(sources.begin(), sources.end(), std::string(self ? self : "")),
			      sources.end());
	}

	if (f->single) { // standalone filter: the one module's parameters, straight in the dialog
		const px::ModuleType &mt = *f->single;
		ObsStore one(settings);
		int nbands = px::kEqBands;
		if (mt.type == "eq") {
			double n = mt.params[0].def;
			one.getNum("m1.nbands", n);
			nbands = (int)n;
		}
		for (size_t i = 0; i < mt.params.size(); i++) {
			if (mt.type == "eq" && (int)i >= px::kEqFirstBand + nbands * px::kEqBandParams)
				break;
			if (!mt.params[i].hidden)
				addParam(props, settings, "m1", mt, mt.params[i], sources);
		}
		return;
	}

	// With the editor available the dialog stays short: the editor button, the preset list
	// and one checkbox. Everything else appears only when "show all" is ticked.
	const bool all = obs_data_get_bool(settings, "px_show_all");
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		const std::string n = obs_property_name(p);
		if (n != "px_open" && n != "px_preset" && n != "px_show_all" && n != "px_warn")
			obs_property_set_visible(p, all);
	}

	ObsStore st(settings);
	const std::vector<std::string> ids = px::chainIds(st);
	obs_property_t *sel = obs_properties_get(props, "px_sel");
	obs_property_list_clear(sel);
	int number = 0;
	for (const std::string &id : ids) {
		std::string type;
		st.getStr(id + ".type", type);
		const px::ModuleType *mt = px::findType(type);
		if (!mt)
			continue;
		const std::string title =
			std::to_string(++number) + ". " + tr("Px.Module." + mt->type, mt->name.c_str());
		obs_property_list_add_string(sel, title.c_str(), id.c_str());
		if (!all)
			continue;

		obs_properties_t *g = obs_properties_create();
		int nbands = px::kEqBands;
		if (mt->type == "eq") {
			double n = mt->params[0].def;
			st.getNum(id + ".nbands", n);
			nbands = (int)n;
		}
		for (size_t i = 0; i < mt->params.size(); i++) {
			if (mt->type == "eq" && (int)i >= px::kEqFirstBand + nbands * px::kEqBandParams)
				break; // only the bands in use get controls
			if (!mt->params[i].hidden)
				addParam(g, settings, id, *mt, mt->params[i], sources);
		}
		obs_data_set_default_bool(settings, (id + ".on").c_str(), true);
		obs_properties_add_group(props, (id + ".on").c_str(), title.c_str(), OBS_GROUP_CHECKABLE, g);
	}
	obs_property_set_visible(obs_properties_get(props, "px_warn"), px::limiterMisplaced(st));
}

struct Edit {
	Filter *f;
	obs_data_t *settings;
	ObsStore st;
	std::string sel;
	explicit Edit(void *data)
		: f(static_cast<Filter *>(data)),
		  settings(obs_source_get_settings(f->source)),
		  st(settings)
	{
		sel = f->single ? "m1" : obs_data_get_string(settings, "px_sel");
	}
	// Push the edited settings to the audio side and redraw the module groups.
	bool done(obs_properties_t *props)
	{
		obs_source_update(f->source, nullptr);
		rebuildModules(props, f, settings);
		return true;
	}
	~Edit() { obs_data_release(settings); }
};

bool onPreset(void *data, obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	Filter *f = static_cast<Filter *>(data);
	const std::string path = obs_data_get_string(settings, "px_preset");
	if (path.empty())
		return false;
	for (const PresetInfo &p : listPresets())
		if (p.path == path)
			applyPresetFile(f, settings, p);
	obs_data_set_string(settings, "px_preset", "");
	rebuildModules(props, f, settings);
	return true;
}

bool onImport(void *data, obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	Filter *f = static_cast<Filter *>(data);
	const std::string path = obs_data_get_string(settings, "px_import");
	if (path.empty())
		return false;
	obs_data_set_string(settings, "px_import", "");
	px::FlatMap m;
	if (!readPreset(path, m))
		return false;
	// keep a copy with the user presets, then load it
	const std::string dir = moduleDir(true);
	const std::string name = m.count("_name") ? m["_name"].s : "Imported preset";
	os_mkdirs(dir.c_str());
	m["_name"] = px::Val::str(name);
	const std::string json = px::toFlatJson(m);
	os_quick_write_utf8_file((dir + "/" + safeFileName(name) + ".json").c_str(), json.c_str(), json.size(), false);
	ObsStore st(settings);
	px::applyChain(st, m);
	f->current = name;
	fillPresetList(obs_properties_get(props, "px_preset"), true);
	rebuildModules(props, f, settings);
	return true;
}

bool onExport(void *, obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	std::string path = obs_data_get_string(settings, "px_export");
	if (path.empty())
		return false;
	obs_data_set_string(settings, "px_export", "");
	if (path.size() < 5 || path.compare(path.size() - 5, 5, ".json") != 0)
		path += ".json";
	std::string name = path.substr(path.find_last_of("/\\") + 1);
	name = name.substr(0, name.size() - 5);
	if (!writePreset(path, settings, name))
		obs_log(LOG_WARNING, "could not write preset %s", path.c_str());
	return true;
}

bool onSave(obs_properties_t *props, obs_property_t *, void *data)
{
	Edit e(data);
	const std::string name = obs_data_get_string(e.settings, "px_preset_name");
	if (name.empty())
		return false;
	const std::string dir = moduleDir(true);
	os_mkdirs(dir.c_str());
	if (!writePreset(dir + "/" + safeFileName(name) + ".json", e.settings, name))
		obs_log(LOG_WARNING, "could not save preset %s", name.c_str());
	e.f->current = name;
	fillPresetList(obs_properties_get(props, "px_preset"), true);
	for (int i = 1; i <= kSlots; i++)
		if (obs_property_t *slot = obs_properties_get(props, ("px_slot" + std::to_string(i)).c_str()))
			fillPresetList(slot, false);
	return true;
}

bool onAdd(obs_properties_t *props, obs_property_t *, void *data)
{
	Edit e(data);
	const std::string id = px::addModule(e.st, obs_data_get_string(e.settings, "px_add_type"));
	if (id.empty())
		return false;
	obs_data_set_string(e.settings, "px_sel", id.c_str());
	return e.done(props);
}
bool onRemove(obs_properties_t *props, obs_property_t *, void *data)
{
	Edit e(data);
	return px::removeModule(e.st, e.sel) && e.done(props);
}
bool onUp(obs_properties_t *props, obs_property_t *, void *data)
{
	Edit e(data);
	return px::moveModule(e.st, e.sel, -1) && e.done(props);
}
bool onDown(obs_properties_t *props, obs_property_t *, void *data)
{
	Edit e(data);
	return px::moveModule(e.st, e.sel, 1) && e.done(props);
}
bool onBandAdd(obs_properties_t *props, obs_property_t *, void *data)
{
	Edit e(data);
	return px::addEqBand(e.st, e.sel) && e.done(props);
}
bool onBandRemove(obs_properties_t *props, obs_property_t *, void *data)
{
	Edit e(data);
	const int band = (int)obs_data_get_int(e.settings, "px_band") - 1;
	return px::removeEqBand(e.st, e.sel, band) && e.done(props);
}
bool onLearn(obs_properties_t *props, obs_property_t *, void *data)
{
	// A changed token tells every Denoise module to capture a new noise profile.
	Edit e(data);
	bool any = false;
	for (const std::string &id : px::chainIds(e.st)) {
		std::string type;
		e.st.getStr(id + ".type", type);
		if (type != "denoise")
			continue;
		double token = 0;
		e.st.getNum(id + ".learn", token);
		e.st.set(id + ".learn", px::Val::num(token + 1));
		any = true;
	}
	return any && e.done(props);
}

#ifdef PX_ENABLE_EDITOR
// ------------------------------------------------------------------ editor window
std::vector<QPointer<QWidget>> g_editors;

class ObsHost : public px::EditorHost {
public:
	explicit ObsHost(Filter *f) : link(f->link), settings(obs_source_get_settings(f->source)), st(settings) {}
	~ObsHost() override { obs_data_release(settings); }

	bool alive() override
	{
		return with<bool>(false, [](Filter *) { return true; });
	}
	bool singleModule() override
	{
		return with<bool>(false, [](Filter *f) { return f->single != nullptr; });
	}
	px::Store &store() override { return st; }
	void changed() override
	{
		with<int>(0, [](Filter *f) {
			obs_source_update(f->source, nullptr);
			return 0;
		});
	}
	void gestureEnded() override
	{
		with<int>(0, [](Filter *f) {
			obs_source_update_properties(f->source); // keeps an open Filters dialog in step
			return 0;
		});
	}
	std::shared_ptr<px::Module> module(const std::string &id) override
	{
		return with<std::shared_ptr<px::Module>>(nullptr, [&](Filter *f) { return f->chain.module(id); });
	}
	int latencySamples() override
	{
		return with<int>(0, [](Filter *f) { return f->chain.latencySamples(); });
	}
	double sampleRate() override
	{
		return with<double>(48000, [](Filter *f) { return f->sr; });
	}
	int channels() override
	{
		return with<int>(2, [](Filter *f) { return f->ch; });
	}
	double cpuPercent() override
	{
		return with<double>(0, [](Filter *f) { return (double)f->cpu.load(); });
	}
	bool enabled() override
	{
		return with<bool>(false, [](Filter *f) { return obs_source_enabled(f->source); });
	}
	void setEnabled(bool on) override
	{
		with<int>(0, [on](Filter *f) {
			obs_source_set_enabled(f->source, on);
			return 0;
		});
	}
	std::vector<px::PresetEntry> presets() override
	{
		std::vector<px::PresetEntry> out;
		for (const PresetInfo &p : listPresets())
			out.push_back({p.path, p.name, p.category});
		return out;
	}
	bool loadPreset(const std::string &id) override
	{
		return with<bool>(false, [&](Filter *f) {
			for (const PresetInfo &p : listPresets())
				if (p.path == id) {
					applyPresetFile(f, settings, p);
					return true;
				}
			return false;
		});
	}
	bool savePreset(const std::string &name) override
	{
		const std::string dir = moduleDir(true);
		os_mkdirs(dir.c_str());
		return writePreset(dir + "/" + safeFileName(name) + ".json", settings, name);
	}
	std::vector<px::PresetEntry> modulePresets(const std::string &type) override
	{
		std::vector<px::PresetEntry> out;
		for (const PresetInfo &p : listPresets(type))
			out.push_back({p.path, p.name, p.category});
		return out;
	}
	bool loadModulePreset(const std::string &presetId, const std::string &moduleId) override
	{
		px::FlatMap m;
		return readPreset(presetId, m) && px::applyModule(st, moduleId, m);
	}
	bool saveModulePreset(const std::string &moduleId, const std::string &name) override
	{
		px::FlatMap m = px::captureModule(st, moduleId);
		if (!m.count("_type"))
			return false;
		m["_name"] = px::Val::str(name);
		const std::string dir = moduleDir(true, "module-presets");
		os_mkdirs(dir.c_str());
		const std::string json = px::toFlatJson(m),
				  path = dir + "/" + m["_type"].s + "--" + safeFileName(name) + ".json";
		return os_quick_write_utf8_file(path.c_str(), json.c_str(), json.size(), false);
	}
	std::vector<std::string> audioSources() override
	{
		std::vector<std::string> sources;
		obs_enum_sources(enumAudioSources, &sources);
		const std::string self = sourceName();
		sources.erase(std::remove(sources.begin(), sources.end(), self), sources.end());
		return sources;
	}
	std::string sourceName() override
	{
		return with<std::string>("", [](Filter *f) {
			obs_source_t *parent = obs_filter_get_parent(f->source);
			const char *n = parent ? obs_source_get_name(parent) : nullptr;
			return std::string(n ? n : "");
		});
	}
	int loopState() override
	{
		return with<int>(0, [](Filter *f) { return f->loop.load(); });
	}
	void setLoop(int state) override
	{
		with<int>(0, [state](Filter *f) {
			if (state == 1) { // ten seconds of the dry input; never written to disk
				std::lock_guard<std::mutex> lock(f->scMutex);
				f->loopLen = (size_t)(10.0 * f->sr);
				for (int c = 0; c < f->ch; c++)
					f->loopBuf[c].assign(f->loopLen, 0.0f);
			}
			f->loop.store(state);
			return 0;
		});
	}

private:
	template<typename R, typename F> R with(R fallback, F fn)
	{
		std::lock_guard<std::mutex> lock(link->m);
		return link->f ? fn(link->f) : fallback;
	}
	std::shared_ptr<Filter::Link> link;
	obs_data_t *settings;
	ObsStore st;
};

bool onOpenEditor(obs_properties_t *, obs_property_t *, void *data)
{
	Filter *f = static_cast<Filter *>(data);
	if (!f->editor) {
		f->editor = px::createEditor(std::make_unique<ObsHost>(f),
					     static_cast<QWidget *>(obs_frontend_get_main_window()));
		g_editors.push_back(f->editor);
	}
	f->editor->show();
	f->editor->raise();
	f->editor->activateWindow();
	return false;
}

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	// Editor code lives in this module, so no window may survive until the module unloads.
	if (event != OBS_FRONTEND_EVENT_EXIT && event != OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN)
		return;
	for (QPointer<QWidget> &w : g_editors)
		delete w.data();
	g_editors.clear();
}
#endif

bool onShowAll(void *data, obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	rebuildModules(props, static_cast<Filter *>(data), settings);
	return true;
}

obs_properties_t *filterProperties(void *data)
{
	Filter *f = static_cast<Filter *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_data_t *settings = obs_source_get_settings(f->source);

	if (f->single) {
#ifdef PX_ENABLE_EDITOR
		obs_properties_add_button2(props, "px_open", T("Px.Editor.Open"), onOpenEditor, f);
#endif
		if (f->single->type == "eq") {
			obs_properties_add_button2(props, "px_band_add", T("Px.Eq.AddBandSingle"), onBandAdd, f);
			obs_properties_add_int(props, "px_band", T("Px.Eq.BandNumber"), 1, px::kEqBands, 1);
			obs_data_set_default_int(settings, "px_band", 1);
			obs_properties_add_button2(props, "px_band_remove", T("Px.Eq.RemoveBandSingle"), onBandRemove,
						   f);
		}
		if (f->single->type == "denoise")
			obs_properties_add_button2(props, "px_learn", T("Px.Denoise.Learn"), onLearn, f);
		rebuildModules(props, f, settings);
		obs_data_release(settings);
		return props;
	}

#ifdef PX_ENABLE_EDITOR
	obs_properties_add_button2(props, "px_open", T("Px.Editor.Open"), onOpenEditor, f);
	obs_data_set_default_bool(settings, "px_show_all", false);
#else
	obs_data_set_default_bool(settings, "px_show_all", true); // no editor in this build: the dialog is the UI
#endif

	obs_property_t *p = obs_properties_add_list(props, "px_preset", T("Px.Preset"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	fillPresetList(p, true);
	obs_property_set_modified_callback2(p, onPreset, f);

	p = obs_properties_add_bool(props, "px_show_all", T("Px.ShowAll"));
	obs_property_set_modified_callback2(p, onShowAll, f);

	obs_properties_add_text(props, "px_preset_name", T("Px.Preset.Name"), OBS_TEXT_DEFAULT);
	obs_properties_add_button2(props, "px_save", T("Px.Preset.Save"), onSave, f);
	p = obs_properties_add_path(props, "px_import", T("Px.Preset.Import"), OBS_PATH_FILE,
				    "Parametrix preset (*.json)", nullptr);
	obs_property_set_modified_callback2(p, onImport, f);
	p = obs_properties_add_path(props, "px_export", T("Px.Preset.Export"), OBS_PATH_FILE_SAVE,
				    "Parametrix preset (*.json)", nullptr);
	obs_property_set_modified_callback2(p, onExport, f);

	p = obs_properties_add_list(props, "px_add_type", T("Px.Rack.AddType"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	for (const px::ModuleType &mt : px::registry())
		if (!mt.first && !mt.last)
			obs_property_list_add_string(p, tr("Px.Module." + mt.type, mt.name.c_str()), mt.type.c_str());
	obs_data_set_default_string(settings, "px_add_type", "comp");
	obs_properties_add_button2(props, "px_add", T("Px.Rack.Add"), onAdd, f);

	obs_properties_add_list(props, "px_sel", T("Px.Rack.Selected"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_properties_add_button2(props, "px_up", T("Px.Rack.Earlier"), onUp, f);
	obs_properties_add_button2(props, "px_down", T("Px.Rack.Later"), onDown, f);
	obs_properties_add_button2(props, "px_remove", T("Px.Rack.Remove"), onRemove, f);
	obs_properties_add_button2(props, "px_band_add", T("Px.Eq.AddBand"), onBandAdd, f);
	obs_properties_add_int(props, "px_band", T("Px.Eq.BandNumber"), 1, px::kEqBands, 1);
	obs_data_set_default_int(settings, "px_band", 1);
	obs_properties_add_button2(props, "px_band_remove", T("Px.Eq.RemoveBand"), onBandRemove, f);
	obs_properties_add_button2(props, "px_learn", T("Px.Denoise.Learn"), onLearn, f);

	obs_properties_t *slotProps = obs_properties_create();
	for (int i = 1; i <= kSlots; i++) {
		const std::string key = "px_slot" + std::to_string(i);
		const std::string label = std::string(T("Px.Slot")) + " " + std::to_string(i);
		fillPresetList(obs_properties_add_list(slotProps, key.c_str(), label.c_str(), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING),
			       false);
	}
	obs_properties_add_group(props, "px_slots", T("Px.Slots"), OBS_GROUP_NORMAL, slotProps);

	p = obs_properties_add_text(props, "px_warn", T("Px.Warn.Limiter"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);

	rebuildModules(props, f, settings);
	obs_data_release(settings);
	return props;
}

// ------------------------------------------------------------------ hotkeys
void hkToggle(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	Filter *f = static_cast<Filter *>(data);
	if (pressed)
		obs_source_set_enabled(f->source, !obs_source_enabled(f->source));
}
void hkNext(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		loadPresetFromHotkey(static_cast<Filter *>(data), "", 1);
}
void hkPrev(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		loadPresetFromHotkey(static_cast<Filter *>(data), "", -1);
}

void hkSlot(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	const Indexed *s = static_cast<Indexed *>(data);
	if (!pressed)
		return;
	obs_data_t *settings = obs_source_get_settings(s->f->source);
	const std::string name = obs_data_get_string(settings, ("px_slot" + std::to_string(s->index)).c_str());
	obs_data_release(settings);
	if (!name.empty())
		loadPresetFromHotkey(s->f, name, 0);
}

void hkModule(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	const Indexed *s = static_cast<Indexed *>(data);
	if (!pressed)
		return;
	obs_data_t *settings = obs_source_get_settings(s->f->source);
	ObsStore st(settings);
	const std::vector<std::string> ids = px::chainIds(st);
	if (s->index <= (int)ids.size()) {
		const std::string key = ids[(size_t)(s->index - 1)] + ".on";
		double on = 1;
		st.getNum(key, on);
		obs_data_set_bool(settings, key.c_str(), on == 0);
		obs_source_update(s->f->source, nullptr);
		obs_source_update_properties(s->f->source);
	}
	obs_data_release(settings);
}

void *filterCreate(obs_data_t *settings, obs_source_t *source)
{
	Filter *f = new Filter();
	f->source = source;
#ifdef PX_ENABLE_EDITOR
	f->link = std::make_shared<Filter::Link>();
	f->link->f = f;
#endif
	f->current = obs_data_get_string(settings, "px_current");
	if (const char *type = static_cast<const char *>(obs_source_get_type_data(source)))
		f->single = px::findType(type);

	obs_hotkey_register_source(source, "Parametrix.Toggle", T("Px.Hotkey.Toggle"), hkToggle, f);
	if (f->single) { // presets and rack positions belong to the rack filter only
		filterUpdate(f, settings);
		return f;
	}
	obs_hotkey_register_source(source, "Parametrix.NextPreset", T("Px.Hotkey.Next"), hkNext, f);
	obs_hotkey_register_source(source, "Parametrix.PrevPreset", T("Px.Hotkey.Prev"), hkPrev, f);
	for (int i = 0; i < kSlots; i++) {
		f->hkSlots[i] = {f, i + 1};
		const std::string id = "Parametrix.Slot" + std::to_string(i + 1);
		const std::string label = std::string(T("Px.Hotkey.Slot")) + " " + std::to_string(i + 1);
		obs_hotkey_register_source(source, id.c_str(), label.c_str(), hkSlot, &f->hkSlots[i]);
	}
	for (int i = 0; i < px::kMaxModules; i++) {
		f->hkModules[i] = {f, i + 1};
		const std::string id = "Parametrix.Module" + std::to_string(i + 1);
		const std::string label = std::string(T("Px.Hotkey.Module")) + " " + std::to_string(i + 1);
		obs_hotkey_register_source(source, id.c_str(), label.c_str(), hkModule, &f->hkModules[i]);
	}

	filterUpdate(f, settings);
	return f;
}

void filterDestroy(void *data)
{
	Filter *f = static_cast<Filter *>(data);
	{
		std::lock_guard<std::mutex> lock(f->scMutex);
		for (auto &s : f->sc)
			scDetach(s.get());
	}
#ifdef PX_ENABLE_EDITOR
	{
		std::lock_guard<std::mutex> lock(f->link->m);
		f->link->f = nullptr; // the editor notices on its next tick and closes
	}
#endif
	delete f;
}

// Display names of the standalone filters, keyed by module type. Filled once at load.
std::vector<std::pair<std::string, std::string>> g_singleNames;

const char *filterName(void *typeData)
{
	if (const char *type = static_cast<const char *>(typeData))
		for (const auto &n : g_singleNames)
			if (n.first == type)
				return n.second.c_str();
	return T("Px.Name");
}

} // namespace

bool obs_module_load(void)
{
	static struct obs_source_info info = {};
	info.id = "parametrix_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = filterName;
	info.create = filterCreate;
	info.destroy = filterDestroy;
	info.update = filterUpdate;
	info.get_properties = filterProperties;
	info.filter_audio = filterAudio;
	info.video_tick = filterTick;
	obs_register_source(&info);

	// Every module is also its own filter, for people who want just one of them.
	static std::vector<std::string> ids;
	for (const px::ModuleType &mt : px::registry())
		if (!mt.first && !mt.last) {
			ids.push_back("parametrix_" + mt.type);
			g_singleNames.emplace_back(mt.type, std::string("Parametrix ") +
								    tr("Px.Module." + mt.type, mt.name.c_str()));
		}
	size_t k = 0;
	for (const px::ModuleType &mt : px::registry())
		if (!mt.first && !mt.last) {
			struct obs_source_info one = info;
			one.id = ids[k++].c_str();
			one.type_data = const_cast<char *>(mt.type.c_str()); // owned by the registry, never freed
			one.free_type_data = nullptr;
			obs_register_source(&one);
		}
#ifdef PX_ENABLE_EDITOR
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
#endif
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
