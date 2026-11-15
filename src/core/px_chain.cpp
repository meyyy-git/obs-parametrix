#include "px_chain.hpp"

namespace px {

namespace {
constexpr double kFadeSeconds = 0.02;
constexpr int kMaxLatency = 8192;
} // namespace

Chain::Chain()
{
	live.reserve(64);
	next.reserve(64);
	grave.reserve(64);
	trash.reserve(64);
	for (int c = 0; c < kMaxCh; c++) {
		scratch[c].assign(kMaxBlock, 0.0f);
		dryBuf[c].assign(kMaxBlock, 0.0f);
		dryLine[c].resize(kMaxLatency + kMaxBlock);
		scratchPtr[c] = scratch[c].data();
		dryPtr[c] = dryBuf[c].data();
	}
}

void Chain::configure(const ChainCfg &cfg, double sampleRate, int channels)
{
	channels = std::min(std::max(channels, 1), kMaxCh);
	std::vector<std::shared_ptr<void>> dead;
	std::lock_guard<std::mutex> ui(uiMutex);
	if (sampleRate != uiSr || channels != uiCh) {
		instances.clear(); // every module is rebuilt for the new format
		uiSr = sampleRate;
		uiCh = channels;
	}
	auto snap = std::make_shared<Snapshot>();
	snap->sr = sampleRate;
	snap->ch = channels;
	std::map<std::string, std::pair<std::string, std::shared_ptr<Module>>> kept;
	for (const ModuleCfg &m : cfg.modules) {
		std::shared_ptr<Module> mod;
		auto it = instances.find(m.id);
		if (it != instances.end() && it->second.first == m.type) {
			mod = it->second.second;
		} else {
			mod = createModule(m.type);
			if (!mod)
				continue;
			mod->prepare(sampleRate, channels);
		}
		kept[m.id] = {m.type, mod};
		Entry e;
		e.mod = mod;
		e.on = m.on;
		e.values = m.values;
		snap->entries.push_back(std::move(e));
	}
	instances.swap(kept);
	{
		std::lock_guard<std::mutex> lock(swapMutex);
		if (pending)
			dead.push_back(pending);
		pending = snap;
		dead.insert(dead.end(), trash.begin(), trash.end());
		trash.clear();
	}
	// `dead` and `kept` are released here, on the UI thread
}

std::shared_ptr<Module> Chain::module(const std::string &id) const
{
	std::lock_guard<std::mutex> ui(uiMutex);
	auto it = instances.find(id);
	return it == instances.end() ? nullptr : it->second.second;
}

void Chain::bury(std::shared_ptr<Module> &m)
{
	if (grave.size() < grave.capacity())
		grave.push_back(std::move(m));
	m.reset();
}

void Chain::adopt()
{
	std::unique_lock<std::mutex> lock(swapMutex, std::try_to_lock);
	if (!lock.owns_lock() || !pending)
		return;
	// hand retired objects to the UI thread while we hold the lock
	for (auto &g : grave)
		if (trash.size() < trash.capacity())
			trash.push_back(std::move(g));
	grave.clear();
	if (cur && trash.size() < trash.capacity())
		trash.push_back(std::move(cur));
	cur = std::move(pending);
	pending.reset();
	lock.unlock();

	// Old slots that are gone from the new rack fade out where they were.
	const Module *anchor = nullptr;
	for (Slot &s : live) {
		bool keep = false;
		for (const Entry &e : cur->entries)
			keep = keep || e.mod == s.mod;
		if (keep) {
			anchor = s.mod.get();
		} else {
			s.dying = true;
			s.anchor = anchor;
		}
	}
	next.clear();
	auto pushDying = [&](const Module *a) {
		for (Slot &s : live)
			if (s.dying && s.anchor == a && s.mod && (s.gain > 0 || s.tail > 0) &&
			    next.size() < next.capacity())
				next.push_back(s);
	};
	pushDying(nullptr);
	for (const Entry &e : cur->entries) {
		Slot s;
		s.mod = e.mod;
		s.on = e.on;
		s.gain = first && e.on ? 1.0f : 0.0f; // no fade-in when the filter first loads
		for (const Slot &o : live)
			if (o.mod == e.mod && !o.dying) {
				s.gain = o.gain;
				s.muted = o.muted;
				s.tail = o.tail;
			}
		e.mod->setParams(e.values.data());
		if (next.size() < next.capacity())
			next.push_back(s);
		pushDying(e.mod.get());
	}
	for (Slot &s : live)
		bury(s.mod);
	live.swap(next);
	next.clear();
	first = false;
}

void Chain::process(float **io, int channels, int frames, const ScBlock *sc, int nsc)
{
	adopt();
	if (!cur || cur->ch != channels)
		return; // format not configured yet: pass the audio through
	for (int off = 0; off < frames; off += kMaxBlock) {
		const int n = std::min(kMaxBlock, frames - off);
		float *ptr[kMaxCh];
		ScBlock scOff[kMaxSidechains];
		for (int c = 0; c < channels; c++)
			ptr[c] = io[c] + off;
		const int ns = std::min(nsc, kMaxSidechains);
		for (int s = 0; s < ns; s++) {
			scOff[s].nch = sc[s].nch;
			for (int c = 0; c < sc[s].nch; c++)
				scOff[s].ch[c] = sc[s].ch[c] + off;
		}
		block(ptr, n, scOff, ns);
	}
}

void Chain::block(float **io, int n, const ScBlock *sc, int nsc)
{
	const int ch = cur->ch;
	const double sr = cur->sr;

	int lat = 0;
	for (const Slot &s : live)
		if (s.on && !s.dying)
			lat += s.mod->latency();
	lat = std::min(lat, kMaxLatency);
	latency.store(lat);
	for (int c = 0; c < ch; c++)
		for (int i = 0; i < n; i++) {
			dryLine[c].write(io[c][i]);
			dryBuf[c][(size_t)i] = dryLine[c].read(lat + 1);
		}

	Ctx ctx;
	ctx.sc = sc;
	ctx.nsc = nsc;
	ctx.dry = dryPtr;
	ctx.ch = ch;

	const float step = (float)(1.0 / (kFadeSeconds * sr));
	for (Slot &s : live) {
		const bool want = s.on && !s.dying;
		if (!want && s.gain > 0 && s.mod->tailSeconds() > 0) {
			// Reverb and delay: stop feeding them, but let the tail ring out first.
			if (!s.muted) {
				s.mod->setInputMuted(true);
				s.muted = true;
				s.tail = s.mod->tailSeconds() * sr;
			}
			if (s.tail > 0) {
				s.mod->process(io, n, ctx);
				s.tail -= n;
				continue;
			}
		}
		if (want && s.muted) {
			s.mod->setInputMuted(false);
			s.muted = false;
			s.tail = 0;
		}
		if (!want && s.gain <= 0)
			continue; // bypassed modules cost nothing
		if (want && s.gain >= 1) {
			s.mod->process(io, n, ctx);
			continue;
		}
		// crossfade between the unprocessed and the processed signal
		for (int c = 0; c < ch; c++)
			std::memcpy(scratchPtr[c], io[c], sizeof(float) * (size_t)n);
		s.mod->process(io, n, ctx);
		float g = s.gain;
		for (int i = 0; i < n; i++) {
			g = want ? std::min(1.0f, g + step) : std::max(0.0f, g - step);
			for (int c = 0; c < ch; c++)
				io[c][i] = scratchPtr[c][i] * (1.0f - g) + io[c][i] * g;
		}
		s.gain = g;
	}

	// drop modules that have finished fading out
	size_t w = 0;
	for (size_t r = 0; r < live.size(); r++) {
		if (live[r].dying && live[r].gain <= 0 && live[r].tail <= 0) {
			bury(live[r].mod);
			continue;
		}
		if (w != r)
			live[w] = std::move(live[r]);
		w++;
	}
	live.resize(w);
}

} // namespace px
