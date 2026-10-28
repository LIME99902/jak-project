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

#include <array>
#include <cstdint>
#include <memory>

#include "third-party/glad/include/glad/glad.h"

namespace vr {

/*!
 * Touch controller state translated into a PS2 DualShock 2, ready to merge into the game's pad.
 * Button order matches PadData::ButtonIndex (SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT,
 * L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS, SQUARE).
 *
 * Default bindings (Quest Touch):
 *   left stick   = move           right stick       = camera
 *   A            = Cross (jump)    B                 = Circle (punch)
 *   X            = Square (kick)   Y                 = Triangle (vehicles / talk)
 *   right trigger= R1 (shoot)      left trigger      = L1 (crouch / roll)
 *   right grip   = R2 (jet-board)  left grip         = L2 (Dark Jak)
 *   menu button  = Start           left stick click  = Select
 *   click + flick the right stick = D-pad (pick a gun)
 *   swing either hand forward fast = punch
 */
struct VrPadState {
  bool active = false;
  std::array<bool, 16> buttons{};
  // 0..255 per button (pressure-sensitive buttons), indexed like PadData::PressureIndex
  std::array<uint8_t, 12> pressure{};
  // PS2 analog values, 0..255 with 127 neutral, y = 0 is "up"
  uint8_t lx = 127, ly = 127, rx = 127, ry = 127;
  bool left_stick_used = false;   // stick is deflected; override other devices
  bool right_stick_used = false;
};

/*!
 * Tracked pose of the head or a hand, in meters, in the runtime's local space
 * (origin where the head was at startup / last recenter, +Y up, -Z forward).
 */
struct VrPose {
  bool valid = false;
  float pos[3] = {0, 0, 0};
  float quat[4] = {0, 0, 0, 1};  // x, y, z, w
  float lin_vel[3] = {0, 0, 0};
};

struct VrTrackingState {
  VrPose head;
  VrPose hand_aim[2];   // 0 = left, 1 = right. Pointing ray of the controller.
  VrPose hand_grip[2];  // where the hand holds the controller
};

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

  // --- controllers (safe to call from any thread) ---

  // Latest controller state mapped to a DualShock 2. active == false when VR isn't running.
  VrPadState get_pad_state() const;
  // Latest head and hand poses.
  VrTrackingState get_tracking_state() const;
  // Game rumble -> controller vibration. Strengths 0..1, applied on the next frame.
  void set_rumble(float strength);

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// Global instance used by the OpenGL display.
OpenXrSession& get_session();

}  // namespace vr
