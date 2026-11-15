// Parametrix editor window. Depends on Qt and the core only; the OBS plugin and the
// standalone preview both drive it through EditorHost.
#pragma once

#include "px_module.hpp"
#include "px_settings.hpp"

#include <memory>
#include <string>
#include <vector>

class QWidget;

namespace px {

struct PresetEntry {
	std::string id, name, category;
};

class EditorHost {
public:
	virtual ~EditorHost() = default;
	// False once the filter behind the editor is gone; the editor then closes itself.
	virtual bool alive() = 0;
	// True for a standalone one-module filter: no rack, presets, Simple mode or output strip.
	virtual bool singleModule() { return false; }
	virtual Store &store() = 0;
	virtual void changed() = 0;    // settings were edited: apply them to the audio
	virtual void gestureEnded() {} // a drag or click finished: refresh other UI
	virtual std::shared_ptr<Module> module(const std::string &id) = 0;
	virtual int latencySamples() = 0;
	virtual double sampleRate() = 0;
	virtual int channels() = 0;
	virtual double cpuPercent() = 0;
	virtual bool enabled() = 0;
	virtual void setEnabled(bool on) = 0;
	virtual std::vector<PresetEntry> presets() = 0;
	virtual bool loadPreset(const std::string &id) = 0; // writes the preset into store()
	virtual bool savePreset(const std::string &name) = 0;
	// Presets for one module only; they also serve the standalone one-module filters.
	virtual std::vector<PresetEntry> modulePresets(const std::string & /*type*/) { return {}; }
	virtual bool loadModulePreset(const std::string & /*presetId*/, const std::string & /*moduleId*/)
	{
		return false;
	}
	virtual bool saveModulePreset(const std::string & /*moduleId*/, const std::string & /*name*/) { return false; }
	virtual std::vector<std::string> audioSources() = 0;
	virtual std::string sourceName() = 0;
	// Capture loop: 0 idle, 1 recording, 2 playing.
	virtual int loopState() = 0;
	virtual void setLoop(int state) = 0;
};

// The window owns the host. It deletes itself when closed.
QWidget *createEditor(std::unique_ptr<EditorHost> host, QWidget *parent);

} // namespace px
