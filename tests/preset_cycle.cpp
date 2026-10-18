/*
 *  Copyright (C) 2026 M0Rf30
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

// Drives CLacteaPlayer (the addon's preset logic) against a real lactea engine
// with simulated time and audio, and checks that presets rotate, navigate, lock
// and survive rebuilds correctly. Needs a GPU (lavapipe is enough); exits 77
// (CTest SKIP) when no engine can be created.

#include "LacteaPlayer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace
{

int g_failures = 0;

#define CHECK(cond, ...)                                                                           \
  do                                                                                               \
  {                                                                                                \
    if (!(cond))                                                                                   \
    {                                                                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);                       \
      std::fprintf(stderr, "" __VA_ARGS__);                                                        \
      std::fprintf(stderr, "\n");                                                                  \
      ++g_failures;                                                                                \
    }                                                                                              \
  } while (0)

constexpr uint32_t kW = 160;
constexpr uint32_t kH = 90;
constexpr double kFps = 60.0;

const char* kPresetTemplate = R"(MILKDROP_PRESET_VERSION=100
PSVERSION=0
[preset00]
fRating=3.000000
fDecay=0.980000
nWaveMode=6
fWaveAlpha=1.000000
fWaveScale=0.500000
zoom=1.000000
rot=0.000000
cx=0.500000
cy=0.500000
wave_r=%f
wave_g=0.600000
wave_b=0.200000
wave_x=0.500000
wave_y=0.500000
)";

void WritePreset(const fs::path& file, float red)
{
  char buf[1024];
  std::snprintf(buf, sizeof(buf), kPresetTemplate, red);
  std::ofstream(file) << buf;
}

// Simulated playback clock shared by every phase so time stays monotonic, as
// it is in the addon.
struct Sim
{
  CLacteaPlayer& p;
  double t = 0.0;
  uint32_t frame = 0;
  std::vector<uint8_t> pixels;

  // Run `seconds`, returning the sequence of distinct active-preset indices.
  std::vector<int> Run(double seconds)
  {
    std::vector<int> seq{p.ActivePreset()};
    float pcm[2 * 735];
    const int frames = static_cast<int>(seconds * kFps);
    for (int f = 0; f < frames; ++f, ++frame)
    {
      t += 1.0 / kFps;
      for (int i = 0; i < 735; ++i)
        pcm[2 * i] = pcm[2 * i + 1] = 0.5f * std::sin(static_cast<float>(frame * 735 + i) * 0.05f);
      p.PushAudio(pcm, 735);
      if (!p.Render(t, frame, pixels))
      {
        CHECK(false, "render failed at t=%.2f", t);
        break;
      }
      const int pos = p.ActivePreset();
      if (pos != seq.back())
        seq.push_back(pos);
    }
    return seq;
  }
};

CLacteaPlayer::Settings BaseSettings(const fs::path& dir)
{
  CLacteaPlayer::Settings s;
  s.presetDir = dir.string();
  s.shuffle = false;
  s.smoothDuration = 1.0;
  s.presetDuration = 1.0;
  return s;
}

void TestSequentialRotation(const fs::path& dir)
{
  CLacteaPlayer p([](CLacteaPlayer::LogLevel, const std::string& m)
                  { std::fprintf(stderr, "  [lactea] %s\n", m.c_str()); });
  if (!p.Create(BaseSettings(dir), kW, kH, 44100))
  {
    std::puts("SKIP: cannot create a lactea engine (no GPU?)");
    std::exit(77);
  }

  std::vector<std::string> names;
  CHECK(p.GetPresets(names), "preset list empty");
  CHECK(names.size() == 4, "expected 4 presets, got %zu", names.size());
  CHECK(names.size() == 4 && names[0] == "a" && names[3] == "d", "names not sorted/stripped");
  CHECK(p.ActivePreset() == 0, "sequential start must be preset 0, got %d", p.ActivePreset());

  Sim sim{p};
  const auto seq = sim.Run(40.0);
  std::set<int> seen(seq.begin(), seq.end());
  CHECK(seen.size() == 4, "auto rotation visited %zu of 4 presets", seen.size());
  CHECK(seq.size() >= 8, "only %zu switches in 40 s of 1 s presets", seq.size() - 1);
  for (size_t i = 1; i < seq.size(); ++i)
    CHECK(seq[i] == (seq[i - 1] + 1) % 4, "step %d -> %d is not sequential", seq[i - 1], seq[i]);
}

void TestNavigation(const fs::path& dir)
{
  CLacteaPlayer p;
  CHECK(p.Create(BaseSettings(dir), kW, kH, 44100), "create");
  Sim sim{p};

  CHECK(p.Load(2), "Load(2)");
  CHECK(p.ActivePreset() == 2, "after Load(2) active=%d", p.ActivePreset());
  CHECK(p.Next() && p.ActivePreset() == 3, "Next -> %d", p.ActivePreset());
  CHECK(p.Next() && p.ActivePreset() == 0, "Next wraps -> %d", p.ActivePreset());
  CHECK(p.Previous() && p.ActivePreset() == 3, "Previous wraps -> %d", p.ActivePreset());
  CHECK(!p.Load(-1), "Load(-1) must fail");
  CHECK(!p.Load(4), "Load(4) must fail (out of range)");
  CHECK(p.ActivePreset() == 3, "failed Load moved the playlist to %d", p.ActivePreset());

  // Random never repeats the playing preset and covers every other entry.
  std::set<int> seen;
  for (int i = 0; i < 200; ++i)
  {
    const int before = p.ActivePreset();
    CHECK(p.Random(), "Random");
    const int after = p.ActivePreset();
    CHECK(after >= 0 && after < 4 && after != before, "Random %d -> %d", before, after);
    seen.insert(after);
  }
  CHECK(seen.size() == 4, "Random reached only %zu of 4 presets", seen.size());

  sim.Run(2.0); // presets must still render after all the manual jumps
}

void TestLock(const fs::path& dir)
{
  CLacteaPlayer p;
  CHECK(p.Create(BaseSettings(dir), kW, kH, 44100), "create");
  Sim sim{p};

  sim.Run(2.0);
  CHECK(p.Lock(true) && p.Locked(), "lock");
  CHECK(sim.Run(15.0).size() == 1, "preset changed while locked");
  CHECK(p.Next(), "manual Next must work while locked");
  CHECK(p.Lock(false) && !p.Locked(), "unlock");
  CHECK(sim.Run(15.0).size() > 1, "rotation did not resume after unlock");
}

void TestShuffle(const fs::path& dir)
{
  CLacteaPlayer::Settings s = BaseSettings(dir);
  s.shuffle = true;
  CLacteaPlayer p;
  CHECK(p.Create(s, kW, kH, 44100), "create");
  Sim sim{p};

  const auto seq = sim.Run(40.0);
  std::set<int> seen(seq.begin(), seq.end());
  CHECK(seen.size() >= 3, "shuffle visited only %zu presets", seen.size());
  for (int pos : seq)
    CHECK(pos >= 0 && pos < 4, "position %d out of range", pos);

  p.SetShuffle(false);
  const auto ordered = sim.Run(30.0);
  for (size_t i = 1; i < ordered.size(); ++i)
    CHECK(ordered[i] == (ordered[i - 1] + 1) % 4, "shuffle off: %d -> %d", ordered[i - 1],
          ordered[i]);
}

void TestBrokenPresetIsSkipped(const fs::path& root)
{
  const fs::path broken = root / "broken";
  fs::create_directories(broken);
  WritePreset(broken / "ok1.milk", 0.2f);
  std::ofstream(broken / "bad.milk") << "[preset00]\nper_frame_1=this is ((( not valid;\n"
                                        "per_pixel_1=zoom = = ;\n";
  WritePreset(broken / "ok2.milk", 0.8f);

  CLacteaPlayer p;
  CHECK(p.Create(BaseSettings(broken), kW, kH, 44100), "create");
  Sim sim{p};
  const auto seq = sim.Run(30.0);
  CHECK(seq.size() >= 4, "rotation stalled on a broken preset (%zu switches)", seq.size() - 1);
  std::vector<std::string> names;
  CHECK(p.GetPresets(names) && !names.empty(), "playlist emptied by one bad preset");
  for (int pos : seq)
    CHECK(pos >= 0, "fell back to idle (%d)", pos);
}

void TestPresetDirReload(const fs::path& dir, const fs::path& root)
{
  const fs::path other = root / "other";
  fs::create_directories(other);
  WritePreset(other / "x.milk", 0.5f);
  WritePreset(other / "y.milk", 0.6f);

  CLacteaPlayer p;
  CHECK(p.Create(BaseSettings(dir), kW, kH, 44100), "create");
  Sim sim{p};
  sim.Run(1.0);

  CHECK(p.SetPresetDir(other.string()), "SetPresetDir");
  std::vector<std::string> names;
  CHECK(p.GetPresets(names) && names.size() == 2 && names[0] == "x", "list not replaced (%zu)",
        names.size());
  const auto seq = sim.Run(15.0);
  for (int pos : seq)
    CHECK(pos == 0 || pos == 1, "position %d outside the new 2-entry list", pos);
  CHECK(seq.size() > 1, "no rotation after reload");

  CHECK(p.SetPresetDir(""), "clear dir");
  names.clear();
  CHECK(!p.GetPresets(names), "list should be empty without a folder");
  CHECK(p.ActivePreset() == -1, "idle preset should report -1, got %d", p.ActivePreset());
  CHECK(!p.Next() && !p.Random() && !p.Load(0), "navigation must fail with no list");
  sim.Run(2.0); // idle preset renders
}

void TestRebuildAndResize(const fs::path& dir)
{
  CLacteaPlayer p;
  CHECK(p.Create(BaseSettings(dir), kW, kH, 44100), "create");
  Sim sim{p};
  sim.Run(1.0);

  // A failed Create must leave the running engine untouched.
  CHECK(!p.Create(BaseSettings(dir), 0, 0, 44100), "zero-size create should fail");
  CHECK(p.Active() && p.Width() == kW, "failed create clobbered the engine");
  sim.Run(1.0);

  // Lock survives a rebuild (new compat / sample rate) and a resize.
  CHECK(p.Lock(true), "lock");
  CLacteaPlayer::Settings s = BaseSettings(dir);
  s.compat = LACTEA_COMPAT_MILKDROP2;
  CHECK(p.Create(s, kW, kH, 48000), "rebuild");
  CHECK(p.SampleRate() == 48000 && p.Locked(), "rebuild lost state");
  CHECK(sim.Run(10.0).size() == 1, "preset changed while locked after rebuild");
  CHECK(p.Resize(200, 120) && p.Width() == 200 && p.Height() == 120, "resize");
  sim.Run(1.0);
  CHECK(sim.pixels.size() == 200u * 120u * 4u, "frame buffer not resized (%zu)", sim.pixels.size());

  // Unlocked again, a rebuilt engine keeps rotating.
  CHECK(p.Lock(false), "unlock");
  CHECK(sim.Run(20.0).size() > 1, "no rotation after rebuild");
}

} // namespace

int main()
{
  // `root` holds sibling folders so the recursive scan of `dir` never sees them.
  const fs::path root = fs::temp_directory_path() / "lactea_preset_cycle";
  const fs::path dir = root / "main";
  fs::remove_all(root);
  fs::create_directories(dir);
  const char* names[] = {"a", "b", "c", "d"};
  for (int i = 0; i < 4; ++i)
    WritePreset(dir / (std::string(names[i]) + ".milk"), 0.2f + 0.2f * static_cast<float>(i));

  TestSequentialRotation(dir);
  TestNavigation(dir);
  TestLock(dir);
  TestShuffle(dir);
  TestBrokenPresetIsSkipped(root);
  TestPresetDirReload(dir, root);
  TestRebuildAndResize(dir);

  fs::remove_all(root);
  if (g_failures)
  {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::puts("all preset checks passed");
  return 0;
}
