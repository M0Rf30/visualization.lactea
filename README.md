# visualization.lactea addon for Kodi

A [Kodi](https://kodi.tv) music visualization addon backed by
[Lactea](https://github.com/M0Rf30/lactea) — a pure-Rust MilkDrop 2/3 and
projectM-compatible visualizer built on `wgpu`.

[![Build](https://github.com/M0Rf30/visualization.lactea/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/M0Rf30/visualization.lactea/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/M0Rf30/visualization.lactea)](https://github.com/M0Rf30/visualization.lactea/releases)
[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL%20v2+-blue.svg)](LICENSE.md)

This is the Lactea counterpart of
[`visualization.projectm`](https://github.com/xbmc/visualization.projectm):
instead of wrapping the C++ `libprojectM`, it links Lactea's C ABI
([`lactea-capi`](https://github.com/M0Rf30/lactea/tree/main/crates/lactea-capi))
and renders its frames onto Kodi's surface.

Lactea also ships as a Rust library, a standalone desktop app and a
[browser demo](https://m0rf30.github.io/lactea/); see the
[Lactea README](https://github.com/M0Rf30/lactea#readme) for those, for the
fidelity measurements against projectM and for the C API reference.

## What it does

* Reads the whole `.milk` preset format — custom waves and shapes, per-frame and
  per-vertex code, warp and composite shaders — with MilkDrop 2, MilkDrop 3 or
  projectM semantics (auto-detected per preset by default).
* Rotates presets automatically from a folder of your choice (or the system
  projectM preset folder when none is set), with soft cuts, shuffle, optional
  beat-triggered cuts and a built-in idle preset as the last fallback.
* Reacts to Kodi's audio (interleaved float PCM) and to the preset controls
  Kodi exposes (next / previous / random / load / lock).

## Install

Each [release](https://github.com/M0Rf30/visualization.lactea/releases) ships
`visualization.lactea-<version>.zip` for Kodi 21 (Omega) on Linux x86_64
(built on Ubuntu 24.04, so glibc 2.39 or newer), with Lactea linked in
statically. In Kodi, allow *Unknown sources* (*Settings → System → Add-ons*),
then *Settings → Add-ons → Install from zip file*, pick the zip and select
**Lactea** under *Settings → Player → Music → Visualisation*. A Vulkan driver
is required at runtime (Mesa's lavapipe works in software). For presets,
install your distribution's projectM preset package or set *Preset folder* in
the addon settings.

## How it works

Unlike `visualization.projectm`, Lactea does not draw into Kodi's OpenGL context.
It owns a `wgpu` device (Vulkan, Metal or DX12) and renders into an offscreen
target; `lactea_render_to_buffer` copies each frame back as tight RGBA8. The
addon uploads those pixels into a GL texture and blits a fullscreen quad using a
tiny shader (`resources/shaders/GL` and `GLES`). This keeps the integration
portable across Kodi's GL and GLES render paths at the cost of a CPU read-back
per frame. The render height is capped (setting *Maximum render height*,
1080p by default) and the result is scaled to the screen, so 4K displays do not
read back 33 MB per frame. Window resizes are followed with `lactea_resize`.

Preset management lives in `src/LacteaPlayer.{h,cpp}`, a Kodi-independent
wrapper over the Lactea engine and playlist (`lactea_playlist_*`): a playlist
connected to the engine scans the preset folder, answers the engine's switch
requests and drives automatic rotation. Setting changes apply to the running
engine in place; only the compatibility mode needs a new engine, which is built
once on the next frame and replaces the old one only if creation succeeds. The
lock state survives rebuilds. `src/Main.cpp` is the thin Kodi/GL layer.

## Kodi version

This addon targets **Kodi 21 (Omega)**, which exposes the visualization API as
`Start()` / `Stop()` / `GetSyncDelay()`. Kodi 22+ (master) renamed these to
`AudioStart()` / `AudioStop()` / `AudioGetSyncDelay()` and dropped `Init()` /
`DeInit()` in favor of them; targeting master requires renaming those three
overrides. CI builds against the `Omega` branch.

## Build

You need the Kodi dev headers and a Lactea `liblactea_capi` + `lactea.h`
(Lactea **0.2.2** or newer). The Linux tarballs on the
[Lactea releases page](https://github.com/M0Rf30/lactea/releases) ship both
(`lib/liblactea_capi.{so,a}`, `include/lactea.h`). The `lactea-capi` crate is
not published to crates.io, so without a pre-built library it is compiled from
a Lactea checkout with `cargo`.

### Against an installed Kodi (e.g. Arch `kodi-dev` + `lactea-bin`)

```sh
git clone https://github.com/M0Rf30/visualization.lactea.git
cd visualization.lactea && mkdir build && cd build
cmake -DKodi_DIR=/usr/lib/kodi/cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$HOME/.kodi/addons \
      -DOVERRIDE_PATHS=1 ..
make install
```

### Against the Kodi source tree (and a Lactea checkout)

```sh
git clone --branch Omega https://github.com/xbmc/xbmc.git
git clone https://github.com/M0Rf30/visualization.lactea.git
git clone --depth 1 --branch v0.2.2 https://github.com/M0Rf30/lactea.git

# The addon is not in Kodi's binary-addons list; give the framework a definition.
mkdir -p xbmc/cmake/addons/addons/visualization.lactea
echo "visualization.lactea $PWD/visualization.lactea" \
  > xbmc/cmake/addons/addons/visualization.lactea/visualization.lactea.txt
echo all > xbmc/cmake/addons/addons/visualization.lactea/platforms.txt

# Kodi's addon framework does not forward -D options to the addon, so pass the
# Lactea checkout through the environment.
export LACTEA_SOURCE_DIR=$PWD/lactea
cmake -B visualization.lactea/build \
      -DADDONS_TO_BUILD=visualization.lactea \
      -DADDON_SRC_PREFIX=$PWD \
      -DADDONS_DEFINITION_DIR=$PWD/xbmc/cmake/addons/addons \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$PWD/xbmc/kodi-build/addons \
      -DPACKAGE_ZIP=1 \
      xbmc/cmake/addons
make -C visualization.lactea/build
```

`Findlactea.cmake` runs `cargo build --release -p lactea-capi` against
`LACTEA_SOURCE_DIR` at configure time (a no-op when up to date), or otherwise
looks for a pre-built library and `lactea.h` (`ADDON_DEPENDS_PATH`, a standard
library path, or `LACTEA_LIBRARY` / `LACTEA_INCLUDE_DIR`).

**Linking.** A static `liblactea_capi.a` is preferred and makes the addon
self-contained. With only the shared library, a copy found outside the system
library directories (e.g. a cargo `target/release`) is installed next to the
addon binary and located through an `$ORIGIN` rpath; a system-installed copy is
left to the distribution to provide.

> **Cross-compilation note.** Kodi's `depends` framework has no `cargo` target,
> so the Rust build is driven by CMake. Building for Android or Windows needs
> the corresponding Rust target installed (`rustup target add …`) and the right
> `CARGO_TARGET_*` toolchain, which the stock `depends` flow does not set up.
> Only Linux desktop builds are exercised.

## Tests

`tests/preset_cycle.cpp` drives `CLacteaPlayer` against a real Lactea engine
with simulated time and audio, and checks automatic rotation order, shuffle,
next / previous / random / load, locking, a broken preset not stalling the
rotation, folder reload, and engine rebuild / resize. It needs a GPU (Vulkan
lavapipe is enough) and exits 77 (skip) if no engine can be created.

```sh
cmake -DLACTEA_BUILD_TESTS=ON ... && make && ctest --output-on-failure
```

## Settings

| setting | effect |
|---|---|
| Preset folder | folder of `.milk` / `.milk2` / `.prjm` presets (recursive); empty → first existing of `/usr/share/projectM/presets`, `/usr/local/share/projectM/presets`, `/usr/share/projectm/presets`, `/usr/share/lactea/presets`, `~/.projectM/presets`, else the built-in idle preset |
| Texture folder | images presets reference (`sampler_*`); optional |
| Shuffle mode | pick presets in random order |
| Smooth preset duration | seconds a soft cut blends old and new preset |
| Preset duration | seconds a preset runs before switching |
| Beat-triggered preset changes | let a loud beat cut to the next preset early |
| Beat sensitivity | how loud a beat must be to cut (maps to Lactea's `hard_cut_sensitivity`, 3.0 … 1.0) |
| Compatibility mode | auto / projectM / MilkDrop 2 / MilkDrop 3 (rebuilds the engine) |
| Maximum render height | cap on the rendered frame height; the frame is scaled to the screen |

## License

GPL-2.0-or-later, see `LICENSE.md`. Lactea itself is MIT OR Apache-2.0.
