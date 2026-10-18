/*
 *  Copyright (C) 2026 M0Rf30
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Main.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace
{

// Fullscreen quad, two triangles. Each vertex is (x, y, u, v). lactea hands
// back tight RGBA8 with row 0 = top, which in a GL texture is v = 0, so the
// UVs are arranged to keep the image upright (top of image -> top of screen).
constexpr float kQuad[6][4] = {
    {-1.0f, 1.0f, 0.0f, 0.0f},  // top-left
    {1.0f, 1.0f, 1.0f, 0.0f},   // top-right
    {-1.0f, -1.0f, 0.0f, 1.0f}, // bottom-left
    {1.0f, 1.0f, 1.0f, 0.0f},   // top-right
    {-1.0f, -1.0f, 0.0f, 1.0f}, // bottom-left
    {1.0f, -1.0f, 1.0f, 1.0f},  // bottom-right
};

// Longest gap between two frames fed to the engine. Kodi stops calling Render()
// while paused or hidden; without the clamp the resume would jump the preset
// timers (and audio analysis) by the whole pause.
constexpr double kMaxFrameDelta = 0.25;

// The configured preset folder, or - when the setting is empty - the first
// well-known system preset folder that exists (the one projectM packages use).
std::string EffectivePresetDir(const std::string& configured)
{
  if (!configured.empty())
    return configured;

  std::vector<std::string> candidates = {
      "/usr/share/projectM/presets", "/usr/local/share/projectM/presets",
      "/usr/share/projectm/presets", "/usr/share/lactea/presets"};
  if (const char* home = std::getenv("HOME"))
    candidates.push_back(std::string(home) + "/.projectM/presets");

  std::error_code ec;
  for (const std::string& dir : candidates)
  {
    if (std::filesystem::is_directory(dir, ec))
    {
      kodi::Log(ADDON_LOG_INFO, "Lactea: no preset folder set, using %s", dir.c_str());
      return dir;
    }
  }
  return configured;
}

} // namespace

CVisualizationLactea::CVisualizationLactea()
    : m_player(
          [](CLacteaPlayer::LogLevel level, const std::string& msg)
          {
            kodi::Log(level == CLacteaPlayer::LogLevel::Error ? ADDON_LOG_ERROR : ADDON_LOG_INFO,
                      "Lactea: %s", msg.c_str());
          })
{
  m_settings.presetDir = EffectivePresetDir(kodi::addon::GetSettingString("preset_folder"));
  m_settings.textureDir = kodi::addon::GetSettingString("texture_folder");
  m_settings.shuffle = kodi::addon::GetSettingBoolean("shuffle");
  m_settings.smoothDuration = kodi::addon::GetSettingInt("smooth_duration");
  m_settings.presetDuration = kodi::addon::GetSettingInt("preset_duration");
  m_settings.beatSensitivity = static_cast<float>(kodi::addon::GetSettingInt("beat_sens")) / 50.0f;
  m_settings.hardCut = kodi::addon::GetSettingBoolean("hard_cut");
  m_settings.compat = static_cast<LacteaCompat>(kodi::addon::GetSettingInt("compat_mode"));
  m_maxHeight = kodi::addon::GetSettingInt("max_height");
}

CVisualizationLactea::~CVisualizationLactea()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_player.Destroy();
}

bool CVisualizationLactea::Start(int channels, int samplesPerSec, int bitsPerSample,
                                 const std::string& songName)
{
  (void)bitsPerSample;
  (void)channels; // lactea only analyses the first two channels; Kodi downmixes.
  (void)songName;

  std::lock_guard<std::mutex> lock(m_mutex);

  m_sampleRate = samplesPerSec > 0 ? samplesPerSec : 44100;
  m_buildFailed = false;
  m_renderErrorLogged = false;
  // The clock keeps running across tracks (engine time must not go backwards);
  // only the frame delta is restarted so a gap between tracks is not counted.
  m_haveLastFrame = false;

  // (Re)create the engine when it does not exist yet, the sample rate changed
  // or a setting asked for it. Otherwise the current preset just carries on.
  if (!m_player.Active() || m_needRebuild ||
      m_player.SampleRate() != static_cast<uint32_t>(m_sampleRate))
  {
    if (!CreateEngine(Width(), Height()) && !m_player.Active())
      return false;
  }
  return true;
}

void CVisualizationLactea::Stop()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  // Free GL resources while the context is still current. The engine is kept
  // so the last preset resumes on the next Start().
  FreeGL();
}

void CVisualizationLactea::AudioData(const float* audioData, size_t audioDataLength)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  // Kodi supplies interleaved stereo float PCM; lactea consumes sample *frames*.
  m_player.PushAudio(audioData, audioDataLength / 2);
}

void CVisualizationLactea::QueryViewport(int& width, int& height)
{
  GLint vp[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, vp);
  width = vp[2];
  height = vp[3];
  if (width <= 0 || height <= 0)
  {
    width = Width();
    height = Height();
  }
  if (width <= 0 || height <= 0)
  {
    width = 1280;
    height = 720;
  }
}

void CVisualizationLactea::RenderSize(int viewportWidth, int viewportHeight, uint32_t& width,
                                      uint32_t& height) const
{
  double w = std::max(1, viewportWidth);
  double h = std::max(1, viewportHeight);
  if (m_maxHeight > 0 && h > m_maxHeight)
  {
    w = std::round(w * m_maxHeight / h);
    h = m_maxHeight;
  }
  width = static_cast<uint32_t>(std::max(1.0, w));
  height = static_cast<uint32_t>(std::max(1.0, h));
}

bool CVisualizationLactea::CreateEngine(int viewportWidth, int viewportHeight)
{
  uint32_t w = 0;
  uint32_t h = 0;
  RenderSize(viewportWidth, viewportHeight, w, h);

  m_needRebuild = false;
  if (!m_player.Create(m_settings, w, h, static_cast<uint32_t>(m_sampleRate)))
  {
    m_buildFailed = true;
    return false;
  }
  return true;
}

void CVisualizationLactea::Render()
{
  std::lock_guard<std::mutex> lock(m_mutex);

  int vpW = 0;
  int vpH = 0;
  QueryViewport(vpW, vpH);

  if ((!m_player.Active() || m_needRebuild) && !m_buildFailed)
    CreateEngine(vpW, vpH);
  if (!m_player.Active())
    return;

  if (!m_glReady && !InitGL())
    return;

  // Follow viewport changes (window resize, fullscreen toggle) and the height cap.
  uint32_t w = 0;
  uint32_t h = 0;
  RenderSize(vpW, vpH, w, h);
  if (w != m_player.Width() || h != m_player.Height())
    m_player.Resize(w, h);

  // Advance the clock from real elapsed time.
  const auto now = std::chrono::steady_clock::now();
  if (m_haveLastFrame)
    m_time += std::min(std::chrono::duration<double>(now - m_lastFrame).count(), kMaxFrameDelta);
  m_lastFrame = now;
  m_haveLastFrame = true;

  if (!m_player.Render(m_time, m_frame, m_pixels))
  {
    // The player already logged the cause; do it once, not 60 times a second.
    if (!m_renderErrorLogged)
      kodi::Log(ADDON_LOG_ERROR, "Lactea: frame skipped (further failures not logged)");
    m_renderErrorLogged = true;
    return;
  }
  m_renderErrorLogged = false;
  m_frame++;

  DrawFrame();
}

bool CVisualizationLactea::InitGL()
{
  const std::string vertShader =
      kodi::addon::GetAddonPath("resources/shaders/" GL_TYPE_STRING "/vert.glsl");
  const std::string fragShader =
      kodi::addon::GetAddonPath("resources/shaders/" GL_TYPE_STRING "/frag.glsl");
  if (!LoadShaderFiles(vertShader, fragShader) || !CompileAndLink())
  {
    kodi::Log(ADDON_LOG_ERROR, "Lactea: failed to compile blit shader");
    return false;
  }

  glGenTextures(1, &m_texture);
  glBindTexture(GL_TEXTURE_2D, m_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  m_texWidth = 0; // storage is (re)allocated by DrawFrame() to match the frame
  m_texHeight = 0;

#ifdef HAS_GL
  glGenBuffers(1, &m_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
#endif

  m_glReady = true;
  return true;
}

void CVisualizationLactea::FreeGL()
{
  if (!m_glReady)
    return;
  if (m_texture)
    glDeleteTextures(1, &m_texture);
#ifdef HAS_GL
  if (m_vbo)
    glDeleteBuffers(1, &m_vbo);
  m_vbo = 0;
#endif
  m_texture = 0;
  m_texWidth = 0;
  m_texHeight = 0;
  m_glReady = false;
}

void CVisualizationLactea::DrawFrame()
{
  const uint32_t w = m_player.Width();
  const uint32_t h = m_player.Height();

  glBindTexture(GL_TEXTURE_2D, m_texture);
  if (w != m_texWidth || h != m_texHeight)
  {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    m_texWidth = w;
    m_texHeight = h;
  }
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA,
                  GL_UNSIGNED_BYTE, m_pixels.data());

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);

#ifdef HAS_GL
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
  glVertexAttribPointer(m_hPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
  glEnableVertexAttribArray(m_hPos);
  glVertexAttribPointer(m_hUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        BUFFER_OFFSET(2 * sizeof(float)));
  glEnableVertexAttribArray(m_hUv);
#else
  glVertexAttribPointer(m_hPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), &kQuad[0][0]);
  glEnableVertexAttribArray(m_hPos);
  glVertexAttribPointer(m_hUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), &kQuad[0][2]);
  glEnableVertexAttribArray(m_hUv);
#endif

  EnableShader();
  glDrawArrays(GL_TRIANGLES, 0, 6);
  DisableShader();

  glDisableVertexAttribArray(m_hPos);
  glDisableVertexAttribArray(m_hUv);
#ifdef HAS_GL
  glBindBuffer(GL_ARRAY_BUFFER, 0);
#endif
  glBindTexture(GL_TEXTURE_2D, 0);
}

void CVisualizationLactea::OnCompiledAndLinked()
{
  m_hPos = glGetAttribLocation(ProgramHandle(), "a_position");
  m_hUv = glGetAttribLocation(ProgramHandle(), "a_uv");
  m_uTex = glGetUniformLocation(ProgramHandle(), "u_tex");
}

bool CVisualizationLactea::OnEnabled()
{
  if (m_uTex != -1)
    glUniform1i(m_uTex, 0);
  return true;
}

bool CVisualizationLactea::GetPresets(std::vector<std::string>& presets)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_player.GetPresets(presets);
}

int CVisualizationLactea::GetActivePreset()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_player.ActivePreset();
}

bool CVisualizationLactea::PrevPreset()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_player.Previous();
}

bool CVisualizationLactea::NextPreset()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_player.Next();
}

bool CVisualizationLactea::LoadPreset(int select)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_player.Load(select);
}

bool CVisualizationLactea::RandomPreset()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_player.Random();
}

bool CVisualizationLactea::LockPreset(bool lockUnlock)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_player.Lock(lockUnlock);
}

bool CVisualizationLactea::IsLocked()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_player.Locked();
}

ADDON_STATUS CVisualizationLactea::SetSetting(const std::string& settingName,
                                              const kodi::addon::CSettingValue& settingValue)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  // Every setting except the compat mode applies to the running engine in
  // place; the compat mode needs a new engine, built on the next Render()/Start().
  bool timings = false;
  if (settingName == "preset_folder")
  {
    m_settings.presetDir = EffectivePresetDir(settingValue.GetString());
    m_player.SetPresetDir(m_settings.presetDir);
  }
  else if (settingName == "texture_folder")
  {
    m_settings.textureDir = settingValue.GetString();
    m_player.SetTextureDir(m_settings.textureDir);
  }
  else if (settingName == "shuffle")
  {
    m_settings.shuffle = settingValue.GetBoolean();
    m_player.SetShuffle(m_settings.shuffle);
  }
  else if (settingName == "smooth_duration")
  {
    m_settings.smoothDuration = settingValue.GetInt();
    timings = true;
  }
  else if (settingName == "preset_duration")
  {
    m_settings.presetDuration = settingValue.GetInt();
    timings = true;
  }
  else if (settingName == "beat_sens")
  {
    m_settings.beatSensitivity = static_cast<float>(settingValue.GetInt()) / 50.0f;
    timings = true;
  }
  else if (settingName == "hard_cut")
  {
    m_settings.hardCut = settingValue.GetBoolean();
    timings = true;
  }
  else if (settingName == "compat_mode")
  {
    m_settings.compat = static_cast<LacteaCompat>(settingValue.GetInt());
    m_needRebuild = true;
    m_buildFailed = false;
  }
  else if (settingName == "max_height")
  {
    m_maxHeight = settingValue.GetInt(); // Render() resizes to match
  }
  else
  {
    return ADDON_STATUS_UNKNOWN;
  }

  if (timings)
    m_player.SetTimings(m_settings.smoothDuration, m_settings.presetDuration,
                        m_settings.beatSensitivity, m_settings.hardCut);

  return ADDON_STATUS_OK;
}

ADDONCREATOR(CVisualizationLactea)
