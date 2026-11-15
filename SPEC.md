# Parametrix — Specification

Parametrix is a free, open-source **all-in-one vocal channel strip** for OBS Studio, shipped as a single native audio filter. Inside it is a free rack of DAW-grade modules (denoise, gate, parametric EQ, de-esser, compressor, multiband, saturation, reverb, delay, limiter) that the user adds, removes, and reorders.

- **License:** GPL-2.0-or-later (links against libobs)
- **Repo:** `obs-parametrix` (personal GitHub account; may move to an org later)
- **Status:** audio engine, OBS filter and a first editor window are built. See "Build status" below.

## Build status

| Part | State |
|---|---|
| Audio engine (`src/core`): rack, crossfades, tails, all 13 module types below except Pitch correction | Built. 1390 unit checks pass on Windows. |
| OBS filter: properties dialog generated from the registry, presets (load, save, import, export), 23 factory presets, hotkeys, sidechain capture, en-US and id-ID | Built against libobs 31.1.1. The maintainer loaded it in OBS 33 and reported it working; no checklist run yet. |
| `px-render` CLI, core-tests CI workflow | Built. The CI workflow has not run yet. |
| Editor window (M2): top bar, chain bar with drag reorder, EQ curve with analyzer and band panel, compressor, multiband, output and generic module views, Simple mode macros, presets, A/B, undo/redo, capture loop, UI scale, resizable window | Built. Every view was checked from rendered screenshots of the standalone preview (`editor-preview`). **Mouse and keyboard interaction has not been exercised, and the editor has not been opened inside OBS yet.** |
| Editor gaps against §6 | No keyboard navigation or focus rings for the custom-drawn controls; reverb, delay and trim views are knobs only; the live gain of a dynamic EQ band is not drawn (its range is); the compressor curve has no live input marker; no Auto-gain button |
| Linear-phase EQ, true-peak limiting, auto-gain | Built and unit-tested. Not yet listened to in OBS. |
| Multiband sidechain (one external source drives all four bands with its overall level) | Built, compiled and covered by the preset tests; not listened to. |
| Cramping fix, CPU benchmark gate in CI | Not built |
| Standalone per-module filters (M7): "Parametrix Denoise", "Parametrix Gate", "Parametrix Leveler", "Parametrix EQ", "Parametrix De-esser", "Parametrix Compressor", "Parametrix Multiband", "Parametrix Saturation", "Parametrix Reverb", "Parametrix Delay", "Parametrix Limiter" | Built: each is a rack of exactly one module, with its own short properties dialog and the editor in one-module mode (no rack bar, presets, Simple mode or output strip). The one-module editor was checked from a screenshot. **Not yet loaded in OBS.** They have no presets: module presets (§8) are still not built, so "share presets with the rack" is not met. |
| Pitch correction (M8): key, scale (chromatic, major, minor, two pentatonics), retune speed, amount | Built. Tests pull detuned sines and a harmonic-rich tone onto the right note and leave in-tune notes alone. **Tested on synthetic tones only, never on a real voice.** Costs 0.6% of one core and 960 samples (20 ms) of latency. |
| Module presets: 22 factory presets (EQ, compressor, gate, de-esser, reverb, delay, pitch) in `data/module-presets/`, user presets saved from the editor's "Presets" button in the module header; they work in the rack and in the standalone filters | Built. Core round trip and every factory file are unit-tested. The menu itself has not been clicked through. Not available in the OBS properties dialog, only in the editor. |

Measured CPU on the development machine, stereo 48 kHz, 10 ms blocks (readings vary by about ±0.3 points between runs): the default "Stream vocal" rack uses 1.5% of one core; a rack with all 14 module types uses about 5%. The 2% target in §4 is met for the default rack and **not met** for the all-modules rack. Per module: Multiband 1.3%, Denoise 1.1–1.5%, Gate 0.7%, Pitch 0.6%, Saturation 0.6% (down from 1.0% after skipping the half-band filter's zero taps), Reverb 0.6%.

Known limits of the current build:
- The Denoise noise profile from "Learn noise" is not saved; after a restart the module adapts again on its own.
- Reordering modules while audio plays is not crossfaded (add, remove and bypass are).
- Changing Trim's Align delay while audio plays can click.
- EQ "Flat tilt" is an approximation built from five shelves.
- Pitch correction is for one voice at a time (70 to 800 Hz). It detects pitch with YIN on a 4x-decimated copy and shifts with a pitch-synchronous two-tap delay line. Chords, music and heavy noise will confuse it; consonants pass through unshifted. There is no formant control, so large corrections change the vowel colour slightly.
- Linear-phase EQ: latency is 2304 samples (48 ms at 48 kHz), from a 4096-tap FIR plus one 256-sample block; the 42.7 ms in the mockup was an estimate. In this mode dynamic bands use their static gain, band solo is ignored, and every band applies to all channels (no Left/Right/Mid/Side placement). Changing a band while audio plays swaps the FIR at once, which can click on large moves.
- True-peak limiting reads the level from a 4x oversampled copy and adds 12 samples of latency. Tested to keep the reconstructed peak within 3% of the ceiling; it is not a certified true-peak meter.
- Auto gain matches the chain's output RMS to its input RMS over about 3 s, within ±12 dB. It counts Trim's gain as part of the chain, so it also undoes a Trim change.
- The editor draws everything itself with QPainter (one widget, hit regions rebuilt on each paint). That makes scaling and the look simple, at the cost of accessibility: screen readers see one opaque widget.

## 1. Why this exists

OBS ships separate, basic audio filters: 3-Band EQ, Compressor, Limiter, Expander, Noise Gate, and Noise Suppression. There is no de-esser, no parametric or dynamic EQ, no multiband compressor, no saturation, reverb, or delay, and no way to manage a vocal chain as one unit with one preset and one editor. Parametrix fills that gap.

The **feature references** are FabFilter Pro-Q 3 (EQ) and iZotope Nectar/Neutron (channel strip, Simple mode). They are references only: the name, logo, and UI look must be original, and no third-party product or brand names may be used in naming or branding.

## 2. Platform and tooling

| Item | Decision |
|---|---|
| Language | C++ (Qt6 for the editor window) |
| Base | [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) with `ENABLE_QT` |
| Minimum OBS | 31.1 (the plugin template builds against libobs 31.1.1; OBS refuses plugins built for a newer libobs than itself) |
| Primary dev platform | Windows |
| Release builds | Windows, macOS, Linux via the template's GitHub Actions |
| macOS signing | Not notarized at first. The README explains right-click → Open. Notarize later if sponsored. |
| Distribution | GitHub Releases + OBS Forums → Resources → Plugins. Windows: a one-click Inno Setup installer (`installer/parametrix.iss`, built by CI next to the zip) that installs to `%ProgramData%\obs-studio\plugins` without an administrator prompt and registers an uninstaller. It is unsigned, so SmartScreen warns on first run. macOS `.pkg` and Ubuntu `.deb` come from the template. |
| Locales | `en-US` (required), `id-ID`; others via contributor PRs |

## 3. The rack

- **One OBS filter** ("Parametrix") holds an ordered list of module instances.
- Users can **add** modules (from a "+ Add module" list), **remove**, **duplicate**, **reorder** (drag in the editor, move up/down in properties), and **bypass** them.
- The **same module type can appear more than once**, for example two compressors in series or two EQs.
- **Trim is always first and Output is always last.** Everything in between is free. If a Limiter is not the last module before Output, the editor shows a warning but does not block the order.
- **Maximum of 16 modules** per instance.
- A new instance starts with the **"Stream vocal"** chain, not an empty rack.
- A module that is not in the rack, or is bypassed, costs **zero CPU**.
- Bypass, add, and remove use a **20 ms crossfade**, so there are no clicks while live. Reverb and delay **tails ring out** naturally when bypassed. Compressor and limiter gain reduction release smoothly.

## 4. Modules

Default "Stream vocal" order: Trim → Gate → EQ → De-esser → Compressor → Limiter → Output.

| Module | Controls | Notes |
|---|---|---|
| **Trim** | Gain ±24 dB, invert polarity, input meter, **Align delay** 0–500 ms | Locked first. Align delay is a pure delay (no echo, no feedback) for lining sources up, for example holding a karaoke backing track back so it matches a vocal that arrives late through the mic chain. It is intentional, so it is not counted in the chain latency readout. Set by ear; no auto-detection. |
| **Denoise** | Learn noise (1–2 s), Adaptive (continuously updates the profile), Reduction, Sensitivity, Smoothing; noise profile shown on the EQ analyzer | Non-AI spectral gating. About 10.7 ms latency, only while it is in the rack and enabled |
| **Gate / Expander** | Threshold, range, ratio (1:1 to gate), attack, hold, release, hysteresis, detector HPF/LPF, external sidechain | |
| **Leveler** | Target level, max change (boost and cut limit), speed | Slow automatic volume rider that keeps level even when the speaker moves toward or away from the mic. Works over phrases, where the compressor works over syllables. Never boosts while the Gate is closed or the input is below the noise floor. |
| **EQ** | See §5 | |
| **De-esser** | Frequency, threshold, range, Split-band / Wideband mode, **Listen** (hear only what is removed) | Engine is a dynamic EQ band |
| **Compressor** | Threshold, ratio, knee, attack, release (+Auto), makeup (+Auto), mix (parallel), RMS/Peak detector, detector HPF, external sidechain, gain reduction meter. Characters: **Clean**, **Opto** (program-dependent release) | FET/VCA characters later |
| **Multiband** | 4 bands, Linkwitz-Riley 24 dB/oct crossovers, per-band threshold/ratio/attack/release/makeup, band solo | Zero latency |
| **Saturation** | Drive, type **Tape / Tube / Soft clip**, tone, mix | 2× oversampling against aliasing |
| **Reverb** | Type **Room / Plate / Hall**, size, decay, pre-delay, damping, wet low cut and high cut, mix | Self-written FDN algorithm |
| **Delay** | Time in **ms** (OBS has no tempo), feedback, ping-pong, feedback-path filters, mix | |
| **Limiter** | Ceiling, release (+Auto), 1.5 ms lookahead, optional **true-peak** (4× oversampling) | |
| **Output** | Gain, stereo **width** (0–200%), **balance**, **mono**, **chain mix** (dry/wet for the whole rack), **LUFS** meter (momentary, short-term, integrated) with target −14 / −16 / −23, true-peak readout | Locked last. Width, balance and mono are hidden on mono sources. |
| **Pitch correction** | Key and scale, correction speed, amount | Milestone M8, the hardest module. Monophonic vocal only; adds about 20 ms latency while enabled. |

Not included: **AI noise suppression** (OBS already ships RNNoise and NVIDIA), **de-reverb** and **vocal separation** (poor results without AI). Planned for v2: FET/VCA compressor characters, **voice changer** (pitch and formant shift), **chorus / doubler**, **MIDI control**, and the **Assistant** (listens for about 10 s and sets trim, gate threshold, compressor threshold, and the Denoise profile by level measurement, no AI).

### Hotkeys
Streamers drive OBS from hotkeys and Stream Deck, so each Parametrix instance registers OBS hotkeys:
- Toggle the whole filter on/off.
- Toggle bypass for each module in the rack.
- **Preset slots 1–8:** the user assigns a chain preset to each slot in the editor; a hotkey loads that slot. "Previous preset" and "Next preset" hotkeys as well.
- Loading a preset by hotkey uses the same 20 ms crossfade as any other change.

To verify in M1: whether hotkeys registered on a filter source show up in Settings → Hotkeys on all supported OBS versions. If not, register them as frontend hotkeys named after the source and filter. Everything is also reachable through obs-websocket, because presets and bypass are ordinary settings.

### Shared behavior
- **Sidechain:** each module that supports it (Gate, Compressor, Multiband, dynamic EQ bands) picks its own source. The plugin captures each OBS source **once**, however many modules use it.
- **Stereo:** compressor and limiter detection is **linked** by default. Mono sources stay mono, including reverb and delay output.
- **Latency:** the editor toolbar shows the **total chain latency**. It stays under 2 ms without Denoise (Limiter lookahead is 1.5 ms) and is about 12 ms with Denoise. Linear-phase EQ adds its own latency, which is shown as well. OBS has no automatic latency compensation for filters, so the user sets the source's Sync Offset if needed.
- **CPU target:** one stereo instance at 48 kHz with every module enabled (except linear-phase EQ) uses **under 2% of one mid-range CPU core**. Measured by a benchmark in CI.

## 5. EQ module

- Up to **24 bands**, each with its own enable/bypass toggle.
- **Types:** bell, low shelf, high shelf, low cut, high cut, notch, band-pass, tilt shelf, flat tilt.
- **Slopes** (low/high cut): 6, 12, 18, 24, 36, 48, 72, 96 dB/oct.
- **Parameters:** frequency (10 Hz to just below Nyquist), gain (±30 dB), Q (0.025 to 40). These ranges are initial defaults and may be tuned.
- **Stereo placement** per band: Stereo / Left / Right / Mid / Side. Available only on stereo sources; on mono and surround the band applies to all channels and the control is hidden.
- **Solo** a band to hear it in isolation.
- **Dynamic EQ** per band: threshold, range, attack, release, internal detector or external sidechain.
- **Phase mode:** Zero latency (default) or Linear phase, with its latency shown.
- **Pre/post spectrum analyzer.**

## 6. UI

### Editor window (Qt)
Opened from an "Open editor" button in the filter's properties. It has **no state of its own**: it reads and writes the same OBS settings object, so scene collections, undo, copy/paste filters, and obs-websocket keep working.

- **Top bar:** chain preset browser, A/B compare, undo/redo, **Simple / Advanced** switch, total latency, CPU readout, and a master **power button** at the right end.
- **Master power button** mirrors the filter's enabled state in OBS (the eye icon in the Filters list), so toggling either one updates the other. On: a green power icon with a slow breathing glow (a steady glow when the OS asks for reduced motion). Off: a grey, unlit icon, a "Bypassed" tag, and a greyed-out rack and module area. Audio passes through unprocessed; the editor stays editable.
- **Chain bar:** one card per module in order, with bypass toggle and a mini gain-reduction meter. Click a card to open the module; drag to reorder; "+" at the end adds a module.
- **Module area (Advanced):** the selected module's view.
  - **EQ:** draggable response curve (double-click adds a band, scroll changes Q, Alt-click bypasses, Shift for fine control), floating band panel with knobs and filter-shape buttons, analyzer, band chips. The band panel shows the band's type name, has a trash icon for Delete (never an ×, so it can't be mistaken for close), a **Minimize** button that collapses it to one line with a value summary (the choice persists across bands), and a **Close** button. Esc or a click on empty curve space also closes it.
  - **Compressor:** transfer curve with the live input level, scrolling gain-reduction history, knobs.
  - **Multiband:** the input spectrum split into four coloured zones (Low, Low-mid, High-mid, High). Each crossover is a vertical line with a handle and its frequency, dragged sideways (arrow keys move the selected band's upper crossover; Shift for fine). Crossovers keep at least a third of an octave apart. Each zone shows its live gain reduction as a bar falling from the top. Below it, a strip of four band cards (name, frequency range, gain-reduction meter and value, Solo); clicking a card or a zone selects that band, and soloing one or more bands dims the rest. The knob row edits the selected band: Threshold, Ratio, Attack, Release, Makeup.
  - **Gate, De-esser, Limiter, Denoise:** scrolling gain-reduction history, knobs.
  - **Saturation:** transfer curve. **Reverb:** decay envelope. **Delay:** echo taps.
  - **Output:** LUFS readouts against the selected target.
- **Simple mode:** six macro knobs, each driving several module parameters. Turning a macro adds the module it needs if it is missing.

| Macro | Drives |
|---|---|
| Clean | Gate threshold, Denoise reduction |
| Warmth | EQ low shelf, Saturation drive |
| Presence | EQ presence bell (around 3 kHz) |
| De-ess | De-esser threshold and range |
| Punch | Compressor threshold, ratio, makeup |
| Space | Reverb mix |

- **Output strip:** always visible on the right, with an L/R meter, output gain knob, and Auto-gain.
- **Capture loop:** a top-bar button records 10 s of the dry input, then plays it on repeat through the rack in place of the live input, so one person can talk first and adjust afterwards. OBS cannot route a filter's output to monitoring only, so **the loop also goes to the stream**: a warning banner stays visible while it plays, and the loop stops when the editor closes or the button is pressed again. The buffer is never written to disk.
- **Window:** resizable, with the last size remembered, and a UI scale setting (100 / 125 / 150 / 200%) for high-DPI monitors. Layouts must hold from 900×560 upward. Plan this from the first M2 commit; retrofitting scaling into fixed-size Qt widgets is expensive.

### Visual rules (editor)
- Dark, mostly neutral UI; colour comes from band colours, meters and one amber accent (selection, focus ring, active toggle outline).
- **No violet or lavender** anywhere in the UI or band palette.
- Active toggles (On, Dynamic, Solo, Auto gain) are **outlined in the accent**, not filled, so no view has a wall of filled buttons.
- UI text is **12px minimum**; labels are sentence case, never tracked capitals. Canvas axis labels may go to 10px.
- Text contrast 4.5:1 or better against its background, including hints and dimmed states.
- One radius scale: 6px controls and chips, 8px module cards, 10px panels and the window.
- Every control answers the pointer: hover steps up 1px (fine pointers only), press sinks 1px and scales to .985, keyboard focus shows a 2px accent ring. Under reduced motion nothing moves.
- Icons are drawn as one stroke set (1.6px, round caps); no text glyphs such as ‹ › stand in for icons.
- Buttons say a verb and an object ("Duplicate module", "Learn noise").

### OBS properties dialog
Generated automatically from the parameter registry (§7). At the top, a fixed block of rack controls: preset list, save / import / export, "Add module" (dropdown + button), a "Selected module" list with Move earlier / Move later / Remove, "Add band" and "Remove band" for the selected EQ, "Learn noise", and the eight hotkey preset slots. Below it, one checkable group per module in the rack. The rack controls are never removed while the dialog is open; only the module groups are rebuilt after an edit, so a button never frees its own property. "Open editor" joins the fixed block in M2.

## 7. Parameter registry and settings schema

Every module parameter is declared **once** in a registry: key, label, range, default, unit, scale (linear/log), and smoothing. The registry generates the OBS properties, the settings schema, the preset format, the editor bindings, and the locale keys. Adding a parameter means editing the registry only.

Settings live in the filter's `obs_data_t` as a **flat** key/value set. An earlier draft nested modules in an array; OBS properties and obs-websocket can only address top-level keys, so every parameter is a top-level key instead:

```jsonc
{
  "chain": "m1,m2,m3,m4,m5",        // rack order; Trim first, Output last
  "m1.type": "trim",  "m1.on": true, "m1.gain_db": 0.0, "m1.align_delay_ms": 0.0,
  "m2.type": "gate",  "m2.on": true, "m2.threshold_db": -52,
  "m3.type": "eq",    "m3.on": true, "m3.nbands": 2,
  "m3.b0.type": 3, "m3.b0.freq_hz": 80, "m3.b0.slope": 3,      // choices are stored as an index
  "m3.b1.type": 0, "m3.b1.freq_hz": 3200, "m3.b1.gain_db": 3, "m3.b1.q": 0.9,
  "m3.b1.dyn_on": true, "m3.b1.dyn_threshold_db": -30, "m3.b1.dyn_range_db": -6,
  "m3.b1.dyn_sidechain": "Mic/Aux",                           // an OBS source name, "" = internal
  "m4.type": "comp",  "m4.on": true, "m4.ratio": 3.0, "m4.character": 1,
  "m5.type": "out",   "m5.on": true
}
```

- A module id (`m1`, `m2`, …) is stable for the life of that module instance, so meters, undo and the editor can follow it when it moves.
- A missing parameter takes its registry default, so presets stay short.
- A parameter key is never `type` or `on`; those two belong to the module itself.
- Keys starting with `px_` are dialog controls (selected module, preset slots), not rack state.
- A preset file is the same flat object plus `_name` and `_category`.
- Unknown module types are skipped on load and left in the settings. There is no schema version yet; add one with the first breaking change.

## 8. Presets

- **Chain presets** replace the whole rack. **Module presets** replace one module only, for example a compressor preset that leaves the EQ alone.
- Factory and user presets are JSON. User presets support save, load, import, and export.
- Factory presets live in the plugin's data dir (`obs_module_file("presets/")`). User presets live in its config dir (`obs_module_config_path("presets/")`).
- Contributors add factory presets by PR with JSON only, no C++ changes.

The preset browser groups the 24 factory chain presets into four categories. A preset ships in the milestone that completes its modules (for example "Game audio — tame" needs Multiband, M4). All values are starting points and must be tuned by ear before release.

| Category | Factory chain preset | Used on | Main content |
|---|---|---|---|
| Voice | Stream vocal (balanced) | Mic | Light gate, EQ, de-ess, Opto compressor, limiter |
| Voice | Podcast / Talk | Mic | Tighter compression, LUFS target −16 |
| Voice | Loud gamer | Mic | Fast compressor, hard limiter |
| Voice | Soft-spoken / ASMR | Mic | Gentle expander, light compression, sensitive de-ess |
| Voice | Radio DJ | Mic | Multiband, saturation, thick low end |
| Voice | Singing / Karaoke | Mic | Compressor, plate reverb, ping-pong delay |
| Voice | Singing — tuned | Mic | Singing chain with Pitch correction (chromatic, 60 ms) and plate reverb |
| Voice | Dynamic mic | Dynamic mics that need a lot of gain | Large trim, presence and air boost, Opto compressor |
| Voice | VTuber — bright | Mic | High shelf, light tape saturation, tight compression |
| Voice | Wandering talker | Mic, for people who move around | Leveler ahead of the EQ and compressor |
| Music and game | Music bed — auto duck | Music source | Dynamic EQ band sidechained from the mic; the vocal range dips while you talk |
| Music and game | Game audio — tame | Desktop or game audio | Multiband and limiter so explosions don't bury the voice |
| Music and game | Karaoke backing track | Music source the streamer sings over | Align delay (starts at 120 ms, tune by ear) and a safety limiter |
| Music and game | Discord / voice chat | Voice chat source | Levelling compressor and limiter so quiet and loud friends match |
| Fix | Noisy room | Mic | Denoise, tight gate |
| Fix | Budget mic / headset fix | Cheap mics and headsets | Denoise, cut boxiness at 300–500 Hz, add presence, de-ess |
| Fix | Deep voice fix | Mic | Less boom: low shelf and 250 Hz cut, a little presence |
| Fix | Bright voice fix | Mic | Less thin and harsh: low shelf boost, 3 kHz cut, softer top, de-ess |
| Fix | Plosive tamer | Mic | Dynamic low shelf at 130 Hz that ducks only on "p" and "b" pops |
| Fix | Clean — limiter only | Any source | Safety limiter, nothing else |
| FX | FX — Telephone | Mic | Narrow EQ, soft clip |
| FX | FX — Megaphone | Mic | Narrower EQ with a 2 kHz peak, heavy soft clip |
| FX | FX — Robot radio | Mic | Band-limited EQ, tube drive, short metallic delay |
| FX | FX — Cave | Mic | Large hall reverb and a long delay |

EQ module presets carry over from the EQ-only design: Voice — Clarity, Voice — Deep / Radio, Voice — Bright, Podcast / Broadcast, Rumble and hum cut (HPF + 50 Hz notch), De-mud, Telephone, De-esser, Music bed duck (sidechain).

## 9. DSP design

- **Precision:** double for filter state and coefficients.
- **EQ filters:** Andrew Simper's (Cytomic) SVF, artifact-free under per-sample modulation (needed for dynamic EQ and de-essing). Higher-order slopes are cascaded sections. Prewarped design in M1; matched-response **cramping fix** (Vicanek) in M6.
- **Smoothing:** every continuous parameter is smoothed over about 20 ms; filter coefficients are recomputed every 16 samples.
- **Dynamics:** shared detector and envelope code across Gate, De-esser, Compressor, Multiband, Limiter, and dynamic EQ.
- **Mid/side:** M = (L+R)/2, S = (L−R)/2, run M/S bands, then decode. Done only when a stereo band uses mid or side.
- **Threading:** a settings update (UI thread) builds an immutable chain snapshot. The audio thread picks it up with an atomic swap and never locks or allocates. Meters go back to the UI through lock-free atomics.
- **Sample rate:** read from OBS's audio output; everything is recomputed when it changes.
- **Linear-phase EQ (M6):** FIR built from the active bands' magnitude response, applied with FFT partitioned convolution.

## 10. Testing

- **DSP unit tests:** EQ magnitude response against the analytic response for each type and slope; compressor and limiter static curves; limiter ceiling never exceeded; gate open/close timing. CTest with plain asserts, no test framework dependency.
- **`px-render` CLI:** `px-render in.wav preset.json out.wav` renders a chain offline without OBS. Used by tests and contributors.
- **CPU benchmark** in CI against the target in §4.
- **Manual checklist in OBS:** mono, stereo, and 5.1 sources; 44.1 and 48 kHz; preset and module changes while live (no clicks); scene collection save and reload; filter copy/paste; obs-websocket parameter changes.
- All automated tests run in CI on every PR.

## 11. Milestones

Each milestone is its own branch and PR, tested in OBS before merge.

| # | Scope | Done when |
|---|---|---|
| **M1** | Template setup, parameter registry, module rack (add/remove/reorder/bypass, crossfades), Trim, static EQ (all types and slopes, 24 bands, stereo placement), Output (gain, width, balance, mono, chain mix, meter), OBS hotkeys (filter, module bypass, preset slots), properties UI, chain and module presets, `px-render`, unit tests, en-US + id-ID | Usable EQ strip in OBS on Windows; CI green on 3 platforms |
| **M2** | Qt editor: chain bar, EQ curve + analyzer, module views, **Simple / Advanced** modes with macros, A/B, undo/redo, capture loop, resizable window with UI scale, preset slot assignment | Editor and properties stay in sync both ways |
| **M3** | Dynamic EQ, **De-esser**, **Compressor**, **Limiter**, sidechain capture | The "Stream vocal" factory chain works end to end |
| **M4** | **Gate / Expander**, **Leveler**, **Multiband**, **Denoise** | "Noisy room" preset works on a real fan-noise recording |
| **M5** | **Saturation**, **Reverb**, **Delay** | "Singing / Karaoke" preset works; tails ring out on bypass |
| **M6** | Linear-phase EQ, auto-gain, cramping fix, true-peak limiting, LUFS metering, CPU benchmark gate | CPU target met in CI |
| **M7** | Each module also registered as its own OBS filter ("Parametrix Compressor", etc.) from the same registry | Standalone filters share presets with the rack |
| **M8** | **Pitch correction** module | In-key singing is corrected without audible warble at moderate settings; latency shown |
| v2 | Assistant auto-setup, FET/VCA compressor characters, voice changer, chorus / doubler, MIDI control | — |

## 12. Workflow

- Claude writes the code per milestone; the maintainer reviews and tests in OBS.
- This file is the source of truth. Any change of scope updates SPEC.md in the same PR.
