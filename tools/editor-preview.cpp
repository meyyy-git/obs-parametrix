// Runs the editor without OBS, on a synthetic voice, for development and screenshots.
//   editor-preview [presets-dir] [--shot out.png] [--select <module type>] [--simple]
//                  [--band <n>] [--preset <n>] [--off] [--size WxH] [--scale <1..2>]
#include "editor.hpp"
#include "px_chain.hpp"

#include <QApplication>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace px;

class PreviewHost : public EditorHost {
public:
	explicit PreviewHost(std::string presetDir) : dir(std::move(presetDir))
	{
		ensureChain(st);
		changed();
		QObject::connect(&audio, &QTimer::timeout, [this] { pump(); });
		audio.start(10);
	}
	bool alive() override { return true; }
	bool singleModule() override { return one; }
	void makeSingle(const std::string &type)
	{
		one = true;
		st.map.clear();
		st.set("chain", Val::str("m1"));
		st.set("m1.type", Val::str(type));
		changed();
	}
	Store &store() override { return st; }
	void changed() override { chain.configure(readChain(st), 48000, 2); }
	std::shared_ptr<Module> module(const std::string &id) override { return chain.module(id); }
	int latencySamples() override { return chain.latencySamples(); }
	double sampleRate() override { return 48000; }
	int channels() override { return 2; }
	double cpuPercent() override { return 0.9; }
	bool enabled() override { return on; }
	void setEnabled(bool v) override { on = v; }
	std::vector<PresetEntry> presets() override
	{
		std::vector<PresetEntry> out;
		std::error_code ec;
		std::vector<std::filesystem::path> files;
		for (const auto &e : std::filesystem::directory_iterator(dir, ec))
			if (e.path().extension() == ".json")
				files.push_back(e.path());
		std::sort(files.begin(), files.end());
		for (const auto &f : files) {
			FlatMap m;
			if (!read(f.string(), m))
				continue;
			out.push_back({f.string(), m["_name"].s, m["_category"].s});
		}
		return out;
	}
	bool loadPreset(const std::string &id) override
	{
		FlatMap m;
		if (!read(id, m))
			return false;
		applyChain(st, m);
		return true;
	}
	bool savePreset(const std::string &) override { return true; }
	std::vector<PresetEntry> modulePresets(const std::string &type) override
	{
		std::vector<PresetEntry> out;
		std::error_code ec;
		for (const auto &e : std::filesystem::directory_iterator(
			     std::filesystem::path(dir).parent_path() / "module-presets", ec)) {
			FlatMap m;
			if (e.path().extension() == ".json" && read(e.path().string(), m) && m["_type"].s == type)
				out.push_back({e.path().string(), m["_name"].s, "Factory"});
		}
		return out;
	}
	bool loadModulePreset(const std::string &presetId, const std::string &moduleId) override
	{
		FlatMap m;
		return read(presetId, m) && applyModule(st, moduleId, m);
	}
	std::vector<std::string> audioSources() override { return {"Mic/Aux", "Desktop Audio", "Music"}; }
	std::string sourceName() override { return "Mic/Aux"; }
	int loopState() override { return loop; }
	void setLoop(int s) override { loop = s; }

	MapStore st;

private:
	static bool read(const std::string &path, FlatMap &m)
	{
		std::ifstream f(path);
		std::stringstream ss;
		ss << f.rdbuf();
		return f && parseFlatJson(ss.str(), m);
	}
	// A rough voice: harmonics shaped by two moving formants, syllable bursts, and the odd "s".
	void pump()
	{
		float l[480], r[480], *io[2] = {l, r};
		for (int i = 0; i < 480; i++) {
			t += 1.0 / 48000;
			const double syl = std::max(0.0, std::sin(t * 5.1 * 2)) * (0.55 + 0.45 * std::sin(t * 0.9));
			env += (syl - env) * 0.002;
			const double f0 = 135 + 20 * std::sin(t * 1.6), F1 = 550 + 180 * std::sin(t * 3.4),
				     F2 = 1700 + 450 * std::sin(t * 2.2 + 1);
			phase += f0 / 48000;
			double v = 0;
			for (int h = 1; h <= 30; h++) {
				const double f = f0 * h;
				const double a = (std::exp(-std::pow(std::log2(f / F1), 2) / 0.5) +
						  0.6 * std::exp(-std::pow(std::log2(f / F2), 2) / 0.3) + 0.05) /
						 h;
				v += a * std::sin(2 * 3.141592653589793 * phase * h);
			}
			seed = seed * 1664525u + 1013904223u;
			const double n = ((seed >> 8) & 0xFFFF) / 32768.0 - 1.0;
			const double ess = std::sin(t * 1.8) > 0.8 && syl > 0.3 ? 1.0 : 0.0;
			sib += (ess - sib) * 0.001;
			hp += (n - hp) * 0.4; // crude high-pass for the sibilance
			const double s = 0.5 * env * v + 0.25 * sib * (n - hp) + 0.0015 * n;
			l[i] = (float)s;
			r[i] = (float)(s * 0.92);
		}
		if (on)
			chain.process(io, 2, 480);
	}

	std::string dir;
	Chain chain;
	QTimer audio;
	bool on = true, one = false;
	int loop = 0;
	double t = 0, env = 0, phase = 0, sib = 0, hp = 0;
	unsigned seed = 1;
};

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	std::string dir = "data/presets", shot;
	int preset = -1, w = 1180, h = 720;
	bool off = false;
	auto host = std::make_unique<PreviewHost>(argc > 1 && argv[1][0] != '-' ? argv[1] : dir);
	PreviewHost *hp = host.get();
	for (int i = 1; i < argc; i++) {
		const std::string a = argv[i];
		const std::string v = i + 1 < argc ? argv[i + 1] : "";
		if (a == "--shot")
			shot = v;
		else if (a == "--select")
			hp->st.set("px_debug_select", Val::str(v));
		else if (a == "--single")
			hp->makeSingle(v);
		else if (a == "--simple")
			hp->st.set("px_debug_simple", Val::num(1));
		else if (a == "--band")
			hp->st.set("px_debug_band", Val::num(std::atoi(v.c_str())));
		else if (a == "--preset")
			preset = std::atoi(v.c_str());
		else if (a == "--off")
			off = true;
		else if (a == "--scale")
			hp->st.set("px_ui_scale", Val::num(std::atof(v.c_str())));
		else if (a == "--size")
			std::sscanf(v.c_str(), "%dx%d", &w, &h);
	}
	if (preset >= 0) {
		const auto list = hp->presets();
		if (preset < (int)list.size() && hp->loadPreset(list[(size_t)preset].id)) {
			hp->st.set("px_current", Val::str(list[(size_t)preset].name));
			hp->changed();
		}
	}
	if (off)
		hp->setEnabled(false);
	hp->st.set("px_win_w", Val::num(w));
	hp->st.set("px_win_h", Val::num(h));
	QWidget *ed = createEditor(std::move(host), nullptr);
	ed->show();
	if (!shot.empty())
		QTimer::singleShot(2500, [&] {
			ed->grab().save(QString::fromStdString(shot));
			app.quit();
		});
	return app.exec();
}
