# Parametrix

A free, open-source all-in-one vocal channel strip for OBS Studio. One audio filter holds a rack of modules you add, remove and reorder: Trim, Denoise, Gate, Leveler, EQ (24-band parametric and dynamic), De-esser, Compressor, Multiband, Saturation, Reverb, Delay, Limiter and Output.

The full design is in [SPEC.md](SPEC.md). It also lists what is built and what is not.

## Status

Early development. The audio engine and the OBS filter (properties dialog, presets, hotkeys) build and pass their tests. A first version of the editor window opens from the "Open editor" button in the filter's properties.

## Install (Windows)

Build the plugin (below), then copy the folder `release/obs-parametrix` into `C:\ProgramData\obs-studio\plugins\`, and restart OBS. Parametrix then appears under Filters → Audio filters. Needs OBS Studio 31.1 or newer.

## Build

Requirements on Windows: Visual Studio 2022 (or Build Tools) with the C++ workload, CMake 3.28 or newer. The first configure downloads the OBS sources and prebuilt dependencies into `.deps/`.

```
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
cmake --install build_x64 --prefix release --config RelWithDebInfo
```

macOS and Linux use the `macos` and `ubuntu-x86_64` presets from the OBS plugin template.

## Tests and tools

The audio engine in `src/core` has no OBS dependency and builds on its own:

```
cmake -S src/core -B build_core
cmake --build build_core --config Release
ctest --test-dir build_core -C Release --output-on-failure
```

`px-render in.wav preset.json out.wav` runs a WAV file through a preset without OBS and prints the CPU cost.

The editor also runs without OBS, on a synthetic voice, which is the quickest way to work on its UI:

```
cmake -S src/editor -B build_editor -DCMAKE_PREFIX_PATH=.deps/obs-deps-qt6-2025-07-11-x64
cmake --build build_editor --config Release
build_editor/Release/editor-preview data/presets
```

Qt's `bin` folder must be on `PATH` and `QT_PLUGIN_PATH` must point at its `plugins` folder. `--shot out.png` saves a screenshot and exits; `--select comp`, `--simple`, `--band 2`, `--preset 4` and `--size 900x560` choose what it shows.

## Presets

Factory presets live in `data/presets/` as flat JSON. They are generated from the table in `tools/gen_presets.py`; edit the table and run the script. To contribute a preset, add a row there.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
