#pragma once

/*!
 * @file OpenXrSession.h
 * OpenXR support for the OpenGL renderer.
 *
 * Stage 1: the finished game frame is shown on a large flat screen floating in front of the
 * player (an OpenXR quad layer). Later stages add stereo rendering, head tracking and controllers.
 *
 * Usage, from the render thread with the game's GL context current:
 *   init(window)               once, after the GL context exists
 *   begin_frame()              at the start of every displayed frame
 *   end_frame_with_screen(...) after the game frame was drawn to the window back buffer
 *
 * If no runtime or headset is present, init() fails quietly and every other call is a no-op,
 * so the game keeps working on a normal monitor.
 */

#include <memory>

#include "third-party/glad/include/glad/glad.h"

namespace vr {

class OpenXrSession {
 public:
  OpenXrSession();
  ~OpenXrSession();
  OpenXrSession(const OpenXrSession&) = delete;
  OpenXrSession& operator=(const OpenXrSession&) = delete;

  // Try to start VR. sdl_window is the game's SDL_Window*, whose GL context must be current.
  // Returns false (and logs why) if VR is unavailable or disabled.
  bool init(void* sdl_window);
  void shutdown();

  // True once init() succeeded, even if the headset is currently idle.
  bool is_initialized() const;
  // True while the runtime wants frames from us (headset on, app focused or visible).
  bool is_running() const;

  // Handle runtime events, then wait for the headset's frame timing and begin a frame.
  void begin_frame();

  // Copy a region of the given read framebuffer onto the virtual screen and finish the frame.
  // read_fbo == 0 means the window's back buffer. Safe to call if begin_frame() did nothing.
  void end_frame_with_screen(GLuint read_fbo, int x, int y, int w, int h);

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// Global instance used by the OpenGL display.
OpenXrSession& get_session();

}  // namespace vr
