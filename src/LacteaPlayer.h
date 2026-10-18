/*
 *  Copyright (C) 2026 M0Rf30
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>

extern "C"
{
#include "lactea.h"
}

// Kodi-independent wrapper around a lactea engine + preset playlist.
//
// It owns everything related to *which preset is playing*: building the engine,
// scanning the preset folder, automatic rotation, manual navigation, locking and
// the switch timings. Keeping it free of Kodi headers lets the preset logic be
// exercised by tests/preset_cycle.cpp without a running Kodi.
//
// Not thread-safe; the caller serialises access.
class CLacteaPlayer
{
public:
  enum class LogLevel
  {
    Info,
    Error
  };
  using LogFn = std::function<void(LogLevel, const std::string&)>;

  struct Settings
  {
    std::string presetDir;
    std::string textureDir;
    bool shuffle = true;
    double smoothDuration = 3.0;  // seconds a soft cut blends two presets
    double presetDuration = 20.0; // seconds a preset runs before auto-switching
    float beatSensitivity = 1.0f; // 0..2
    bool hardCut = false;         // allow loud beats to cut to the next preset early
    LacteaCompat compat = LACTEA_COMPAT_AUTO;
  };

  explicit CLacteaPlayer(LogFn log = {});
  ~CLacteaPlayer();
  CLacteaPlayer(const CLacteaPlayer&) = delete;
  CLacteaPlayer& operator=(const CLacteaPlayer&) = delete;

  // Build a fresh engine for `settings`. The previous engine, if any, keeps
  // running until the new one is up; on failure it is left untouched and false
  // is returned. The lock state survives a rebuild.
  bool Create(const Settings& settings, uint32_t width, uint32_t height, uint32_t sampleRate);
  void Destroy();
  bool Active() const { return m_engine != nullptr; }
  uint32_t Width() const { return m_width; }
  uint32_t Height() const { return m_height; }
  uint32_t SampleRate() const { return m_sampleRate; }
  const Settings& GetSettings() const { return m_settings; }

  // Change the render size. The loaded preset keeps running.
  bool Resize(uint32_t width, uint32_t height);

  // `frames` stereo sample frames of interleaved float PCM.
  void PushAudio(const float* interleaved, size_t frames);

  // Render one frame into `rgba` (resized to width*height*4, row 0 = top).
  // `time` must be monotonic seconds. Automatic preset rotation happens here.
  bool Render(double time, uint32_t frame, std::vector<uint8_t>& rgba);

  // Settings that apply to the running engine without a rebuild.
  void SetShuffle(bool shuffle);
  void SetTimings(double smoothDuration, double presetDuration, float beatSensitivity,
                  bool hardCut);
  void SetTextureDir(const std::string& dir);
  // Re-scan `dir` and restart the rotation from the new list.
  bool SetPresetDir(const std::string& dir);

  // Preset navigation (user initiated => hard cut).
  bool GetPresets(std::vector<std::string>& names) const;
  int ActivePreset() const; // -1 when no preset list is loaded (idle preset)
  bool Next();
  bool Previous();
  bool Load(int index);
  bool Random();
  bool Lock(bool locked);
  bool Locked() const { return m_locked; }

  // Basename without extension.
  static std::string DisplayName(const std::string& path);

private:
  void ApplySwitchSettings();
  // Fill the playlist from m_settings.presetDir and start the first preset, or
  // fall back to the idle preset when nothing playable is found.
  void StartPlaylist();
  uint32_t PlaylistSize() const;
  void Log(LogLevel level, const std::string& msg) const;

  LogFn m_log;
  LacteaEngine* m_engine = nullptr;
  LacteaPlaylist* m_playlist = nullptr;
  Settings m_settings;
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  uint32_t m_sampleRate = 0;
  bool m_locked = false;
  std::mt19937 m_rng{std::random_device{}()};
};
