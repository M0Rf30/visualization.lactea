/*
 *  Copyright (C) 2026 M0Rf30
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "LacteaPlayer.h"

#include <algorithm>

CLacteaPlayer::CLacteaPlayer(LogFn log) : m_log(std::move(log)) {}

CLacteaPlayer::~CLacteaPlayer()
{
  Destroy();
}

void CLacteaPlayer::Log(LogLevel level, const std::string& msg) const
{
  if (m_log)
    m_log(level, msg);
}

bool CLacteaPlayer::Create(const Settings& settings, uint32_t width, uint32_t height,
                           uint32_t sampleRate)
{
  LacteaConfig cfg;
  lactea_config_default(&cfg);
  cfg.width = width;
  cfg.height = height;
  cfg.sample_rate = sampleRate;
  cfg.fps = 60;
  cfg.compat = settings.compat;
  cfg.preset_duration = settings.presetDuration;

  // Create the replacement first so a failure leaves the running engine alone.
  LacteaEngine* engine = lactea_create(&cfg);
  if (!engine)
  {
    Log(LogLevel::Error, std::string("engine creation failed: ") + lactea_last_error());
    return false;
  }
  LacteaPlaylist* playlist = lactea_playlist_create(engine);
  if (!playlist)
  {
    Log(LogLevel::Error, std::string("playlist creation failed: ") + lactea_last_error());
    lactea_destroy(engine);
    return false;
  }

  Destroy();
  m_engine = engine;
  m_playlist = playlist;
  m_settings = settings;
  m_width = width;
  m_height = height;
  m_sampleRate = sampleRate;

  if (!m_settings.textureDir.empty())
    lactea_set_texture_dir(m_engine, m_settings.textureDir.c_str());
  lactea_playlist_set_shuffle(m_playlist, m_settings.shuffle);
  ApplySwitchSettings();
  StartPlaylist();

  Log(LogLevel::Info, "engine ready (" + std::to_string(width) + "x" + std::to_string(height) +
                          ", " + std::to_string(sampleRate) + " Hz, " +
                          std::to_string(PlaylistSize()) + " presets)");
  return true;
}

void CLacteaPlayer::Destroy()
{
  // The playlist may be destroyed in either order; do it first so it never
  // outlives the engine it is connected to.
  if (m_playlist)
  {
    lactea_playlist_destroy(m_playlist);
    m_playlist = nullptr;
  }
  if (m_engine)
  {
    lactea_destroy(m_engine);
    m_engine = nullptr;
  }
}

bool CLacteaPlayer::Resize(uint32_t width, uint32_t height)
{
  if (!m_engine)
    return false;
  if (width == m_width && height == m_height)
    return true;
  if (lactea_resize(m_engine, width, height) != LACTEA_STATUS_OK)
  {
    Log(LogLevel::Error, std::string("resize failed: ") + lactea_last_error());
    return false;
  }
  m_width = width;
  m_height = height;
  return true;
}

void CLacteaPlayer::PushAudio(const float* interleaved, size_t frames)
{
  if (m_engine && interleaved && frames > 0)
    lactea_push_pcm_f32(m_engine, interleaved, frames, 2);
}

bool CLacteaPlayer::Render(double time, uint32_t frame, std::vector<uint8_t>& rgba)
{
  if (!m_engine)
    return false;

  const size_t bytes = static_cast<size_t>(m_width) * m_height * 4;
  if (rgba.size() != bytes)
    rgba.resize(bytes);

  if (lactea_render_to_buffer(m_engine, time, frame, rgba.data(), bytes) != LACTEA_STATUS_OK)
  {
    Log(LogLevel::Error, std::string("render failed: ") + lactea_last_error());
    return false;
  }
  return true;
}

void CLacteaPlayer::SetShuffle(bool shuffle)
{
  m_settings.shuffle = shuffle;
  if (m_playlist)
    lactea_playlist_set_shuffle(m_playlist, shuffle);
}

void CLacteaPlayer::SetTimings(double smoothDuration, double presetDuration, float beatSensitivity,
                               bool hardCut)
{
  m_settings.smoothDuration = smoothDuration;
  m_settings.presetDuration = presetDuration;
  m_settings.beatSensitivity = beatSensitivity;
  m_settings.hardCut = hardCut;
  ApplySwitchSettings();
}

void CLacteaPlayer::SetTextureDir(const std::string& dir)
{
  m_settings.textureDir = dir;
  if (m_engine && !dir.empty())
    lactea_set_texture_dir(m_engine, dir.c_str());
}

bool CLacteaPlayer::SetPresetDir(const std::string& dir)
{
  m_settings.presetDir = dir;
  if (!m_engine)
    return false;
  lactea_playlist_clear(m_playlist);
  StartPlaylist();
  return true;
}

void CLacteaPlayer::ApplySwitchSettings()
{
  if (!m_engine)
    return;

  LacteaSwitchSettings sw;
  if (lactea_get_switch_settings(m_engine, &sw) != LACTEA_STATUS_OK)
    return;

  sw.soft_cut_duration = std::max(1.0, m_settings.smoothDuration);
  sw.preset_duration = std::max(1.0, m_settings.presetDuration);
  sw.beat_sensitivity = std::clamp(m_settings.beatSensitivity, 0.0f, 2.0f);
  sw.preset_locked = m_locked;
  // lactea only uses `beat_sensitivity` as stored state; what actually decides a
  // beat-driven cut is the loudness multiple `hard_cut_sensitivity`. Map the
  // 0..2 user scale onto it inversely: more sensitive = lower threshold
  // (1.0 -> lactea's default 2.0, range 3.0 .. 1.0).
  sw.hard_cut_enabled = m_settings.hardCut;
  sw.hard_cut_sensitivity = std::max(1.0f, 3.0f - sw.beat_sensitivity);

  if (lactea_set_switch_settings(m_engine, &sw) != LACTEA_STATUS_OK)
    Log(LogLevel::Error, std::string("switch settings rejected: ") + lactea_last_error());
}

uint32_t CLacteaPlayer::PlaylistSize() const
{
  return m_playlist ? lactea_playlist_size(m_playlist) : 0;
}

void CLacteaPlayer::StartPlaylist()
{
  if (!m_settings.presetDir.empty())
  {
    const uint32_t added =
        lactea_playlist_add_path(m_playlist, m_settings.presetDir.c_str(), true, false);
    if (added == 0)
      Log(LogLevel::Error, "no presets found in '" + m_settings.presetDir + "'");
  }

  if (PlaylistSize() > 0)
  {
    // With shuffle on, "next" picks a random entry; otherwise start at the top.
    if (m_settings.shuffle)
      lactea_playlist_play_next(m_playlist, true);
    else
      lactea_playlist_set_position(m_playlist, 0, true);
  }

  // Unplayable entries are dropped by the playlist as they fail, so re-check.
  if (PlaylistSize() == 0)
  {
    if (lactea_load_idle_preset(m_engine) != LACTEA_STATUS_OK)
      Log(LogLevel::Error, std::string("idle preset failed: ") + lactea_last_error());
  }
}

bool CLacteaPlayer::GetPresets(std::vector<std::string>& names) const
{
  const uint32_t size = PlaylistSize();
  if (size == 0)
    return false;

  char** items = lactea_playlist_items(m_playlist, 0, size);
  if (!items)
    return false;
  for (uint32_t i = 0; i < size && items[i]; ++i)
    names.push_back(DisplayName(items[i]));
  lactea_playlist_free_string_array(items);
  return !names.empty();
}

int CLacteaPlayer::ActivePreset() const
{
  if (PlaylistSize() == 0)
    return -1;
  return static_cast<int>(lactea_playlist_get_position(m_playlist));
}

bool CLacteaPlayer::Next()
{
  if (PlaylistSize() == 0)
    return false;
  lactea_playlist_play_next(m_playlist, true);
  return true;
}

bool CLacteaPlayer::Previous()
{
  if (PlaylistSize() == 0)
    return false;
  lactea_playlist_play_previous(m_playlist, true);
  return true;
}

bool CLacteaPlayer::Load(int index)
{
  const uint32_t size = PlaylistSize();
  if (index < 0 || static_cast<uint32_t>(index) >= size)
    return false;
  lactea_playlist_set_position(m_playlist, static_cast<uint32_t>(index), true);
  return true;
}

bool CLacteaPlayer::Random()
{
  const uint32_t size = PlaylistSize();
  if (size == 0)
    return false;

  uint32_t target = 0;
  if (size > 1)
  {
    // Uniform over every entry except the one already playing.
    const uint32_t current = lactea_playlist_get_position(m_playlist);
    target = std::uniform_int_distribution<uint32_t>(0, size - 2)(m_rng);
    if (target >= current)
      ++target;
  }
  lactea_playlist_set_position(m_playlist, target, true);
  return true;
}

bool CLacteaPlayer::Lock(bool locked)
{
  m_locked = locked;
  if (!m_engine)
    return false;
  ApplySwitchSettings();
  return true;
}

std::string CLacteaPlayer::DisplayName(const std::string& path)
{
  const size_t slash = path.find_last_of("/\\");
  const std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  const size_t dot = base.find_last_of('.');
  return (dot == std::string::npos || dot == 0) ? base : base.substr(0, dot);
}
