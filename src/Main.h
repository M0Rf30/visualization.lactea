/*
 *  Copyright (C) 2026 M0Rf30
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "LacteaPlayer.h"

#include <kodi/addon-instance/Visualization.h>
#include <kodi/gui/gl/Shader.h>

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <stdint.h>

class ATTR_DLL_LOCAL CVisualizationLactea : public kodi::addon::CAddonBase,
                                            public kodi::addon::CInstanceVisualization,
                                            public kodi::gui::gl::CShaderProgram
{
public:
  CVisualizationLactea();
  ~CVisualizationLactea() override;

  bool Start(int channels, int samplesPerSec, int bitsPerSample,
             const std::string& songName) override;
  void Stop() override;
  void AudioData(const float* audioData, size_t audioDataLength) override;

  void Render() override;

  bool GetPresets(std::vector<std::string>& presets) override;
  int GetActivePreset() override;
  bool PrevPreset() override;
  bool NextPreset() override;
  bool LoadPreset(int select) override;
  bool RandomPreset() override;
  bool LockPreset(bool lockUnlock) override;
  bool IsLocked() override;

  ADDON_STATUS SetSetting(const std::string& settingName,
                          const kodi::addon::CSettingValue& settingValue) override;

  void OnCompiledAndLinked() override;
  bool OnEnabled() override;

private:
  // Build (or rebuild) the lactea engine for the current settings and the given
  // viewport. On failure a running engine is left untouched.
  bool CreateEngine(int viewportWidth, int viewportHeight);

  // Render size for a viewport: capped to m_maxHeight, aspect preserved.
  void RenderSize(int viewportWidth, int viewportHeight, uint32_t& width, uint32_t& height) const;

  // Current GL viewport (the control's area), falling back to Width()/Height().
  void QueryViewport(int& width, int& height);

  // Set up the GL shader + quad used to blit the lactea frame onto Kodi's
  // surface. Called lazily on the first Render() (the GL context is only
  // guaranteed to be current inside Render()).
  bool InitGL();
  void FreeGL();

  // Upload `m_pixels` into the GL texture (reallocating it on a size change)
  // and draw the fullscreen quad.
  void DrawFrame();

  std::mutex m_mutex;
  CLacteaPlayer m_player;
  CLacteaPlayer::Settings m_settings;
  int m_maxHeight = 1080; // 0 = unlimited

  // An engine rebuild was requested (compat mode change) and is applied on the
  // next Render()/Start(), so several setting changes cost a single rebuild.
  bool m_needRebuild = false;
  // Creation failed; do not retry every frame, only after Start() or a setting change.
  bool m_buildFailed = false;

  // CPU copy of the last rendered frame (tight RGBA8, row 0 = top).
  std::vector<uint8_t> m_pixels;

  // GL state for the blit.
  GLuint m_texture = 0;
  uint32_t m_texWidth = 0;
  uint32_t m_texHeight = 0;
#ifdef HAS_GL
  GLuint m_vbo = 0;
#endif
  GLint m_hPos = -1;
  GLint m_hUv = -1;
  GLint m_uTex = -1;
  bool m_glReady = false;

  // Render clock: monotonic across tracks and engine rebuilds.
  double m_time = 0.0;
  uint32_t m_frame = 0;
  bool m_haveLastFrame = false;
  std::chrono::steady_clock::time_point m_lastFrame;

  int m_sampleRate = 44100;
  bool m_renderErrorLogged = false;
};
