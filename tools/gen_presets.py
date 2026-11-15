"""Generate the factory chain presets in data/presets/.

Presets are flat JSON (see src/core/px_settings.hpp). Edit the table below and run:
    python tools/gen_presets.py
All values are starting points and need tuning by ear before a release.
"""
import json
import pathlib
import re

OUT = pathlib.Path(__file__).resolve().parent.parent / "data" / "presets"

BELL, LS, HS, LC, HC, NOTCH, BP, TILT, FTILT = range(9)
SLOPE = {6: 0, 12: 1, 18: 2, 24: 3, 36: 4, 48: 5, 72: 6, 96: 7}


def band(type_, f, g=0.0, q=0.7071, slope=12, **dyn):
    b = {"type": type_, "freq_hz": f, "gain_db": g, "q": q, "slope": SLOPE[slope]}
    if dyn:
        b["dyn_on"] = True
        b.update({"dyn_" + k: v for k, v in dyn.items()})
    return b


def eq(*bands):
    p = {"nbands": len(bands)}
    for i, b in enumerate(bands):
        for k, v in b.items():
            p[f"b{i}.{k}"] = v
    return ("eq", p)


VOCAL_EQ = eq(band(LC, 80, slope=24), band(LS, 140, -1.5), band(BELL, 320, -3, 1.2),
              band(BELL, 3200, 3, 0.9), band(HS, 12000, 2))

# The shape of a classic dynamic stage vocal microphone: gentle bass roll-off, a wide presence
# lift around 5 kHz, a small dip near 7.5 kHz, a second lift near 9.5 kHz, and a rolled-off top.
# Approximated from memory of published response charts, not measured. It only copies the
# frequency curve, and it stacks on top of whatever the user's own microphone already does.
STAGE_MIC_EQ = eq(band(LC, 45, slope=12), band(LS, 120, -3), band(BELL, 5000, 4.5, 0.9),
                  band(BELL, 7500, -2, 3), band(BELL, 9500, 3, 2.5), band(HC, 15000, slope=12))

# name, category, [(module type, {param: value}), ...]; Trim and Output are added automatically
PRESETS = [
    ("Stream vocal", "Voice", [("gate", {}), VOCAL_EQ, ("deess", {}), ("comp", {"character": 1}), ("lim", {})]),
    ("Podcast / Talk", "Voice", [
        ("gate", {"threshold_db": -50}),
        eq(band(LC, 70, slope=18), band(LS, 150, 2), band(BELL, 400, -3, 1.4), band(BELL, 2500, 2, 1),
           band(HC, 16000, slope=12)),
        ("deess", {}), ("comp", {"ratio": 4, "threshold_db": -24}), ("lim", {}),
        ("out", {"lufs_target": 1})]),
    ("Loud gamer", "Voice", [
        ("trim", {"gain_db": -6}), ("gate", {"threshold_db": -45}), VOCAL_EQ,
        ("comp", {"ratio": 6, "attack_ms": 2, "release_ms": 80, "threshold_db": -22}),
        ("lim", {"ceiling_db": -2})]),
    ("Soft-spoken / ASMR", "Voice", [
        ("trim", {"gain_db": 6}), ("gate", {"ratio": 2, "range_db": -12, "threshold_db": -60}), VOCAL_EQ,
        ("deess", {"threshold_db": -36, "range_db": -10}),
        ("comp", {"ratio": 2, "threshold_db": -30, "character": 1}), ("lim", {})]),
    ("Radio DJ", "Voice", [
        ("gate", {}),
        eq(band(LC, 60, slope=24), band(LS, 110, 4), band(BELL, 350, -3, 1), band(HS, 9000, 3)),
        ("mb", {}), ("sat", {"drive_db": 8, "curve": 1}), ("comp", {"ratio": 4}), ("lim", {})]),
    ("Singing / Karaoke", "Voice", [
        VOCAL_EQ, ("deess", {}), ("comp", {}),
        ("verb", {"space": 1, "mix": 22, "decay_s": 2.2}), ("delay", {"ping_pong": True, "mix": 14}),
        ("lim", {})]),
    ("Singing — tuned", "Voice", [
        VOCAL_EQ, ("deess", {}), ("pitch", {"speed_ms": 60}), ("comp", {}),
        ("verb", {"space": 1, "mix": 22, "decay_s": 2.2}), ("lim", {})]),
    ("Stage vocal mic", "Voice", [STAGE_MIC_EQ, ("deess", {}), ("comp", {}), ("lim", {})]),
    ("Dynamic mic", "Voice", [
        ("trim", {"gain_db": 12}),
        eq(band(LC, 80, slope=24), band(BELL, 3000, 3, 0.9), band(HS, 11000, 4)),
        ("deess", {}), ("comp", {"character": 1}), ("lim", {})]),
    ("VTuber — bright", "Voice", [
        ("gate", {}),
        eq(band(LC, 120, slope=24), band(BELL, 4500, 3, 1), band(HS, 10000, 4)),
        ("deess", {}), ("sat", {"drive_db": 3, "curve": 0}), ("comp", {"ratio": 5, "threshold_db": -24}),
        ("lim", {})]),
    ("Wandering talker", "Voice", [
        ("gate", {}), ("lvl", {}), VOCAL_EQ, ("deess", {}), ("comp", {"character": 1}), ("lim", {})]),
    ("Music bed — auto duck", "Music and game", [
        eq(band(LC, 30, slope=12),
           band(BELL, 1800, 0, 0.7, threshold_db=-40, range_db=-6, attack_ms=20, release_ms=250,
                sidechain="Mic/Aux")),
        ("lim", {})]),
    ("Game audio — tame", "Music and game", [("mb", {}), ("lim", {"ceiling_db": -3})]),
    ("Karaoke backing track", "Music and game", [("trim", {"align_delay_ms": 120}), ("lim", {"ceiling_db": -3})]),
    ("Discord / voice chat", "Music and game", [
        ("gate", {"threshold_db": -55}),
        ("comp", {"ratio": 4, "threshold_db": -26, "attack_ms": 5, "makeup_db": 6}), ("lim", {})]),
    ("Noisy room", "Fix", [
        ("denoise", {"reduction_db": 18}), ("gate", {"threshold_db": -45, "range_db": -40}), VOCAL_EQ,
        ("comp", {}), ("lim", {})]),
    ("Budget mic / headset fix", "Fix", [
        ("denoise", {}),
        eq(band(LC, 100, slope=24), band(BELL, 400, -4, 1.2), band(BELL, 3500, 3, 1), band(HS, 9000, 2)),
        ("deess", {}), ("comp", {}), ("lim", {})]),
    ("Deep voice fix", "Fix", [
        eq(band(LC, 90, slope=24), band(LS, 160, -4), band(BELL, 250, -3, 1), band(BELL, 4000, 2, 1)),
        ("comp", {}), ("lim", {})]),
    ("Bright voice fix", "Fix", [
        eq(band(LC, 80, slope=18), band(LS, 180, 3), band(BELL, 3000, -3, 1.2), band(HS, 8000, -2)),
        ("deess", {"threshold_db": -34}), ("comp", {}), ("lim", {})]),
    ("Plosive tamer", "Fix", [
        eq(band(LC, 70, slope=18),
           band(LS, 130, 0, 0.7071, threshold_db=-30, range_db=-9, attack_ms=1, release_ms=60),
           band(BELL, 3200, 2, 0.9)),
        ("comp", {}), ("lim", {})]),
    ("Clean — limiter only", "Fix", [("lim", {})]),
    ("FX — Telephone", "FX", [
        eq(band(LC, 400, slope=48), band(BELL, 1500, 4, 1), band(HC, 3400, slope=48)),
        ("sat", {"drive_db": 14, "curve": 2}), ("comp", {"ratio": 6}), ("lim", {})]),
    ("FX — Megaphone", "FX", [
        eq(band(LC, 500, slope=48), band(BELL, 2000, 6, 1), band(HC, 4000, slope=48)),
        ("sat", {"drive_db": 18, "curve": 2}), ("lim", {})]),
    ("FX — Robot radio", "FX", [
        eq(band(LC, 300, slope=24), band(HC, 3000, slope=24)),
        ("sat", {"drive_db": 12, "curve": 1}), ("delay", {"time_ms": 12, "feedback": 60, "mix": 40}),
        ("lim", {})]),
    ("FX — Cave", "FX", [
        VOCAL_EQ, ("verb", {"space": 2, "size": 90, "decay_s": 5, "mix": 45}),
        ("delay", {"time_ms": 420, "feedback": 35, "mix": 18}), ("lim", {})]),
]


# Module presets: (module type, name, params). They replace one module and leave the rest alone.
MODULE_PRESETS = [
    ("eq", "Voice — Clarity", VOCAL_EQ[1]),
    ("eq", "Voice — Deep / Radio",
     eq(band(LC, 60, slope=24), band(LS, 110, 4), band(BELL, 350, -3, 1), band(HS, 9000, 3))[1]),
    ("eq", "Voice — Bright",
     eq(band(LC, 120, slope=24), band(BELL, 4500, 3, 1), band(HS, 10000, 4))[1]),
    ("eq", "Stage vocal mic", STAGE_MIC_EQ[1]),
    ("eq", "Podcast / Broadcast",
     eq(band(LC, 70, slope=18), band(LS, 150, 2), band(BELL, 400, -3, 1.4), band(BELL, 2500, 2, 1),
        band(HC, 16000, slope=12))[1]),
    ("eq", "Rumble and hum cut",
     eq(band(LC, 60, slope=48), band(NOTCH, 50, 0, 8), band(NOTCH, 100, 0, 8), band(NOTCH, 150, 0, 8))[1]),
    ("eq", "De-mud", eq(band(LC, 60, slope=12), band(BELL, 300, -5, 0.8))[1]),
    ("eq", "Telephone", eq(band(LC, 400, slope=48), band(BELL, 1500, 4, 1), band(HC, 3400, slope=48))[1]),
    ("eq", "Flat (one band)", eq(band(BELL, 1000, 0, 1))[1]),
    ("comp", "Gentle levelling", {"threshold_db": -24, "ratio": 2, "attack_ms": 15, "release_ms": 200, "character": 1}),
    ("comp", "Tight voice", {"threshold_db": -22, "ratio": 4, "attack_ms": 5, "release_ms": 100, "makeup_db": 5}),
    ("comp", "Parallel punch", {"threshold_db": -30, "ratio": 8, "attack_ms": 2, "release_ms": 80, "makeup_db": 8, "mix": 40}),
    ("gate", "Light", {"threshold_db": -55, "range_db": -15, "ratio": 2}),
    ("gate", "Tight", {"threshold_db": -45, "range_db": -40, "ratio": 10, "hold_ms": 80}),
    ("deess", "Gentle", {"threshold_db": -26, "range_db": -5}),
    ("deess", "Strong", {"threshold_db": -34, "range_db": -10}),
    ("verb", "Small room", {"space": 0, "size": 30, "decay_s": 0.6, "mix": 12}),
    ("verb", "Vocal plate", {"space": 1, "size": 50, "decay_s": 2.0, "mix": 20}),
    ("verb", "Big hall", {"space": 2, "size": 85, "decay_s": 4.0, "mix": 30}),
    ("delay", "Slapback", {"time_ms": 110, "feedback": 0, "mix": 18}),
    ("delay", "Ping-pong echo", {"time_ms": 320, "feedback": 40, "ping_pong": True, "mix": 20}),
    ("pitch", "Natural", {"speed_ms": 80, "amount": 80}),
    ("pitch", "Hard tune", {"speed_ms": 0, "amount": 100}),
]


def build(name, category, modules):
    # Trim first and Output last, taking their params from the list if given
    trim = next((m for m in modules if m[0] == "trim"), ("trim", {}))
    out = next((m for m in modules if m[0] == "out"), ("out", {}))
    mods = [trim] + [m for m in modules if m[0] not in ("trim", "out")] + [out]
    flat = {"_name": name, "_category": category, "chain": ",".join(f"m{i + 1}" for i in range(len(mods)))}
    for i, (type_, params) in enumerate(mods):
        mid = f"m{i + 1}"
        flat[f"{mid}.type"] = type_
        flat[f"{mid}.on"] = True
        for k, v in params.items():
            flat[f"{mid}.{k}"] = v
    return flat


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.json"):
        old.unlink()
    for i, (name, category, modules) in enumerate(PRESETS):
        slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
        path = OUT / f"{i + 1:02d}-{slug}.json"
        path.write_text(json.dumps(build(name, category, modules), indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {len(PRESETS)} presets to {OUT}")
    mod_out = OUT.parent / "module-presets"
    mod_out.mkdir(parents=True, exist_ok=True)
    for old in mod_out.glob("*.json"):
        old.unlink()
    for type_, name, params in MODULE_PRESETS:
        slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
        flat = {"_name": name, "_type": type_}
        flat.update(params)
        (mod_out / f"{type_}--{slug}.json").write_text(json.dumps(flat, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {len(MODULE_PRESETS)} module presets to {mod_out}")


if __name__ == "__main__":
    main()
