// Offline renderer: run a WAV file through a preset without OBS.
//   px-render in.wav preset.json out.wav
// Reads 16/24/32-bit PCM and 32-bit float WAV; writes 32-bit float.
#include "px_chain.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace px;

static uint32_t u32(const unsigned char *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t u16(const unsigned char *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

struct Wav {
	int sr = 48000, ch = 2;
	std::vector<std::vector<float>> data; // planar
};

static bool readWav(const char *path, Wav &w, std::string &err)
{
	std::ifstream f(path, std::ios::binary);
	std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) {
		err = "not a WAV file";
		return false;
	}
	int fmt = 0, bits = 0;
	size_t pos = 12, dataPos = 0, dataLen = 0;
	while (pos + 8 <= b.size()) {
		const uint32_t len = u32(&b[pos + 4]);
		if (!std::memcmp(&b[pos], "fmt ", 4) && pos + 24 <= b.size()) {
			fmt = u16(&b[pos + 8]);
			w.ch = u16(&b[pos + 10]);
			w.sr = (int)u32(&b[pos + 12]);
			bits = u16(&b[pos + 22]);
			if (fmt == 0xFFFE && pos + 34 <= b.size()) // WAVE_FORMAT_EXTENSIBLE
				fmt = u16(&b[pos + 32]);
		} else if (!std::memcmp(&b[pos], "data", 4)) {
			dataPos = pos + 8;
			dataLen = std::min<size_t>(len, b.size() - dataPos);
		}
		pos += 8 + len + (len & 1);
	}
	const int bytes = bits / 8;
	if (!dataPos || w.ch < 1 || w.ch > kMaxCh ||
	    !((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) {
		err = "unsupported WAV format (need 16/24/32-bit PCM or 32-bit float, up to 8 channels)";
		return false;
	}
	const size_t frames = dataLen / (size_t)(bytes * w.ch);
	w.data.assign((size_t)w.ch, std::vector<float>(frames));
	for (size_t i = 0; i < frames; i++)
		for (int c = 0; c < w.ch; c++) {
			const unsigned char *p = &b[dataPos + (i * (size_t)w.ch + (size_t)c) * (size_t)bytes];
			float v;
			if (fmt == 3) {
				std::memcpy(&v, p, 4);
			} else if (bits == 16) {
				v = (int16_t)u16(p) / 32768.0f;
			} else if (bits == 24) {
				v = (float)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 |
						      (uint32_t)p[2] << 24) >>
					    8) /
				    8388608.0f;
			} else {
				v = (float)((int32_t)u32(p) / 2147483648.0);
			}
			w.data[(size_t)c][i] = v;
		}
	return true;
}

static bool writeWav(const char *path, const Wav &w)
{
	const uint32_t frames = (uint32_t)w.data[0].size(), dataLen = frames * (uint32_t)w.ch * 4;
	std::ofstream f(path, std::ios::binary);
	auto put32 = [&](uint32_t v) {
		f.write((const char *)&v, 4);
	};
	auto put16 = [&](uint16_t v) {
		f.write((const char *)&v, 2);
	};
	f.write("RIFF", 4);
	put32(36 + dataLen);
	f.write("WAVEfmt ", 8);
	put32(16);
	put16(3);
	put16((uint16_t)w.ch);
	put32((uint32_t)w.sr);
	put32((uint32_t)(w.sr * w.ch * 4));
	put16((uint16_t)(w.ch * 4));
	put16(32);
	f.write("data", 4);
	put32(dataLen);
	for (uint32_t i = 0; i < frames; i++)
		for (int c = 0; c < w.ch; c++)
			f.write((const char *)&w.data[(size_t)c][i], 4);
	return (bool)f;
}

int main(int argc, char **argv)
{
	if (argc != 4) {
		std::fprintf(stderr, "usage: px-render in.wav preset.json out.wav\n");
		return 2;
	}
	Wav w;
	std::string err;
	if (!readWav(argv[1], w, err)) {
		std::fprintf(stderr, "%s: %s\n", argv[1], err.c_str());
		return 1;
	}
	std::ifstream pf(argv[2]);
	std::stringstream ss;
	ss << pf.rdbuf();
	FlatMap preset;
	if (!pf || !parseFlatJson(ss.str(), preset, &err)) {
		std::fprintf(stderr, "%s: %s\n", argv[2], pf ? err.c_str() : "cannot open");
		return 1;
	}
	MapStore st;
	applyChain(st, preset);
	const ChainCfg cfg = readChain(st);
	if (!cfg.sidechains.empty())
		std::fprintf(stderr, "note: this preset uses a sidechain source; px-render runs it without one\n");
	Chain chain;
	chain.configure(cfg, w.sr, w.ch);
	const int n = (int)w.data[0].size();
	const auto t0 = std::chrono::steady_clock::now();
	for (int off = 0; off < n; off += 480) { // 10 ms blocks at 48 kHz, like OBS
		float *p[kMaxCh];
		for (int c = 0; c < w.ch; c++)
			p[c] = w.data[(size_t)c].data() + off;
		chain.process(p, w.ch, std::min(480, n - off));
	}
	const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	if (!writeWav(argv[3], w)) {
		std::fprintf(stderr, "%s: cannot write\n", argv[3]);
		return 1;
	}
	std::printf("%d modules, %d Hz, %d ch, %d frames, latency %d samples, %.2f%% of one core\n",
		    (int)cfg.modules.size(), w.sr, w.ch, n, chain.latencySamples(), 100.0 * spent / ((double)n / w.sr));
	return 0;
}
