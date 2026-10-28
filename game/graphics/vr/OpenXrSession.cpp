// Windows headers must come before glad (pulled in by OpenXrSession.h) so APIENTRY is defined once.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>  // IUnknown, used by openxr_platform.h
#define XR_USE_PLATFORM_WIN32
#endif

#include "OpenXrSession.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "common/log/log.h"

#define XR_USE_GRAPHICS_API_OPENGL
#include "third-party/openxr/include/openxr/openxr.h"
#include "third-party/openxr/include/openxr/openxr_platform.h"

namespace vr {

namespace {

// GL enums for swapchain formats (written out so we don't depend on glad's extension set)
constexpr int64_t kGlSrgb8Alpha8 = 0x8C43;
constexpr int64_t kGlRgba8 = 0x8058;

// Size and placement of the virtual screen, in meters, relative to where the head was when VR
// started (the runtime's "local" space). Recentering in the headset moves it in front of you.
constexpr float kScreenWidthM = 3.0f;
constexpr float kScreenDistanceM = 2.2f;

const char* result_str(XrInstance instance, XrResult r) {
  static char buf[XR_MAX_RESULT_STRING_SIZE];
  if (instance != XR_NULL_HANDLE && xrResultToString(instance, r, buf) == XR_SUCCESS) {
    return buf;
  }
  snprintf(buf, sizeof(buf), "XrResult(%d)", (int)r);
  return buf;
}

double now_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// PadData::ButtonIndex values (kept local so this file doesn't depend on the input system)
enum Btn {
  SELECT = 0,
  L3,
  R3,
  START,
  DPAD_UP,
  DPAD_RIGHT,
  DPAD_DOWN,
  DPAD_LEFT,
  L2,
  R2,
  L1,
  R1,
  TRIANGLE,
  CIRCLE,
  CROSS,
  SQUARE
};

// button index -> PadData::PressureIndex, or -1
int pressure_index(int btn) {
  switch (btn) {
    case DPAD_RIGHT: return 0;
    case DPAD_LEFT: return 1;
    case DPAD_UP: return 2;
    case DPAD_DOWN: return 3;
    case TRIANGLE: return 4;
    case CIRCLE: return 5;
    case CROSS: return 6;
    case SQUARE: return 7;
    case L1: return 8;
    case R1: return 9;
    case L2: return 10;
    case R2: return 11;
    default: return -1;
  }
}

// stick tuning
constexpr float kStickDeadzone = 0.15f;
constexpr float kTriggerOn = 0.5f;
constexpr float kGripOn = 0.6f;
// swing-to-punch tuning
constexpr float kPunchSpeed = 2.0f;        // m/s
constexpr float kPunchForwardDot = 0.6f;   // how closely the swing must follow the controller
constexpr double kPunchHoldSec = 0.15;     // how long the punch button is held
constexpr double kPunchCooldownSec = 0.4;

uint8_t stick_to_ps2(float v) {
  // v in -1..1 -> 0..255, 127 neutral
  float out = 127.5f + v * 127.5f;
  if (out < 0.f) out = 0.f;
  if (out > 255.f) out = 255.f;
  return (uint8_t)out;
}

}  // namespace

struct OpenXrSession::Impl {
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local_space = XR_NULL_HANDLE;
  XrSessionState state = XR_SESSION_STATE_UNKNOWN;
  bool session_running = false;  // between xrBeginSession and xrEndSession
  bool initialized = false;

  // per-frame
  bool frame_begun = false;
  XrFrameState frame_state{XR_TYPE_FRAME_STATE};

  // screen swapchain
  XrSwapchain swapchain = XR_NULL_HANDLE;
  int64_t swapchain_format = 0;
  int sc_width = 0;
  int sc_height = 0;
  std::vector<XrSwapchainImageOpenGLKHR> sc_images;
  GLuint blit_fbo = 0;

  // controllers
  XrActionSet action_set = XR_NULL_HANDLE;
  XrPath hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};
  XrAction act_move = XR_NULL_HANDLE, act_look = XR_NULL_HANDLE;
  XrAction act_a = XR_NULL_HANDLE, act_b = XR_NULL_HANDLE, act_x = XR_NULL_HANDLE,
           act_y = XR_NULL_HANDLE, act_menu = XR_NULL_HANDLE;
  XrAction act_trigger[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrAction act_grip[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrAction act_stick_click[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrAction act_aim_pose = XR_NULL_HANDLE, act_grip_pose = XR_NULL_HANDLE;
  XrAction act_haptic = XR_NULL_HANDLE;
  XrSpace aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrSpace grip_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrSpace view_space = XR_NULL_HANDLE;
  bool input_ready = false;

  mutable std::mutex input_mutex;  // guards pad, tracking, rumble
  VrPadState pad;
  VrTrackingState tracking;
  float rumble = 0.f;
  double rumble_time = 0;  // when the game last set the rumble
  bool rumble_on = false;
  double punch_until[2] = {0, 0};
  double punch_ready_at[2] = {0, 0};

  bool check(XrResult r, const char* what) {
    if (XR_FAILED(r)) {
      lg::error("[VR] {} failed: {}", what, result_str(instance, r));
      return false;
    }
    return true;
  }

  bool create_instance() {
    uint32_t ext_count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr))) {
      lg::info("[VR] No OpenXR runtime found. Running without VR.");
      return false;
    }
    std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data());
    bool has_gl = false;
    for (auto& e : exts) {
      if (!strcmp(e.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME)) {
        has_gl = true;
      }
    }
    if (!has_gl) {
      lg::warn("[VR] The OpenXR runtime does not support OpenGL. Running without VR.");
      return false;
    }

    const char* enabled_exts[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(ci.applicationInfo.applicationName, "OpenGOAL VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    strncpy(ci.applicationInfo.engineName, "OpenGOAL", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = enabled_exts;
    if (!check(xrCreateInstance(&ci, &instance), "xrCreateInstance")) {
      instance = XR_NULL_HANDLE;
      return false;
    }

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &props))) {
      lg::info("[VR] OpenXR runtime: {} {}.{}.{}", props.runtimeName,
               XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
               XR_VERSION_PATCH(props.runtimeVersion));
    }
    return true;
  }

  bool get_system() {
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(instance, &gi, &system);
    if (XR_FAILED(r)) {
      lg::info("[VR] No headset connected ({}). Running without VR.", result_str(instance, r));
      return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(instance, system, &sp))) {
      lg::info("[VR] Headset: {}", sp.systemName);
    }
    return true;
  }

  bool create_session(void* sdl_window) {
    // Required before creating an OpenGL session.
    PFN_xrGetOpenGLGraphicsRequirementsKHR get_reqs = nullptr;
    if (!check(xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR",
                                     (PFN_xrVoidFunction*)&get_reqs),
               "get xrGetOpenGLGraphicsRequirementsKHR")) {
      return false;
    }
    XrGraphicsRequirementsOpenGLKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    if (!check(get_reqs(instance, system, &reqs), "xrGetOpenGLGraphicsRequirementsKHR")) {
      return false;
    }
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (XR_MAKE_VERSION(major, minor, 0) < reqs.minApiVersionSupported) {
      lg::error("[VR] OpenGL {}.{} is too old for this headset runtime.", major, minor);
      return false;
    }

#ifdef _WIN32
    XrGraphicsBindingOpenGLWin32KHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    (void)sdl_window;
    binding.hDC = wglGetCurrentDC();
    binding.hGLRC = wglGetCurrentContext();
    if (!binding.hDC || !binding.hGLRC) {
      lg::error("[VR] Could not get the window's OpenGL handles.");
      return false;
    }
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system;
    if (!check(xrCreateSession(instance, &sci, &session), "xrCreateSession")) {
      session = XR_NULL_HANDLE;
      return false;
    }
    // some runtimes switch GL contexts while creating the session; make sure ours is current
    wglMakeCurrent(binding.hDC, binding.hGLRC);
#else
    (void)sdl_window;
    lg::warn("[VR] VR is currently only supported on Windows. Running without VR.");
    return false;
#endif

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.f;
    if (!check(xrCreateReferenceSpace(session, &rs, &local_space), "xrCreateReferenceSpace")) {
      return false;
    }
    return true;
  }

  XrPath path(const char* str) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(instance, str, &p);
    return p;
  }

  XrAction make_action(const char* name,
                       const char* display,
                       XrActionType type,
                       bool per_hand = false) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(ci.localizedActionName, display, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    ci.actionType = type;
    if (per_hand) {
      ci.countSubactionPaths = 2;
      ci.subactionPaths = hand_path;
    }
    XrAction a = XR_NULL_HANDLE;
    check(xrCreateAction(action_set, &ci, &a), name);
    return a;
  }

  struct Binding {
    XrAction action;
    const char* path;
  };

  bool suggest(const char* profile, const std::vector<Binding>& binds) {
    std::vector<XrActionSuggestedBinding> sb;
    for (auto& b : binds) {
      if (b.action != XR_NULL_HANDLE) {
        sb.push_back({b.action, path(b.path)});
      }
    }
    XrInteractionProfileSuggestedBinding isb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    isb.interactionProfile = path(profile);
    isb.countSuggestedBindings = (uint32_t)sb.size();
    isb.suggestedBindings = sb.data();
    XrResult r = xrSuggestInteractionProfileBindings(instance, &isb);
    if (XR_FAILED(r)) {
      lg::warn("[VR] Bindings for {} not accepted: {}", profile, result_str(instance, r));
      return false;
    }
    return true;
  }

  bool create_input() {
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    strncpy(asci.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(asci.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!check(xrCreateActionSet(instance, &asci, &action_set), "xrCreateActionSet")) {
      return false;
    }
    hand_path[0] = path("/user/hand/left");
    hand_path[1] = path("/user/hand/right");

    act_move = make_action("move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT);
    act_look = make_action("camera", "Camera", XR_ACTION_TYPE_VECTOR2F_INPUT);
    act_a = make_action("jump", "Jump (Cross)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    act_b = make_action("punch", "Punch (Circle)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    act_x = make_action("kick", "Spin kick (Square)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    act_y = make_action("interact", "Interact (Triangle)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    act_menu = make_action("pause", "Pause (Start)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    act_trigger[0] = make_action("crouch", "Crouch (L1)", XR_ACTION_TYPE_FLOAT_INPUT);
    act_trigger[1] = make_action("shoot", "Shoot (R1)", XR_ACTION_TYPE_FLOAT_INPUT);
    act_grip[0] = make_action("dark_jak", "Dark Jak (L2)", XR_ACTION_TYPE_FLOAT_INPUT);
    act_grip[1] = make_action("jet_board", "Jet-Board (R2)", XR_ACTION_TYPE_FLOAT_INPUT);
    act_stick_click[0] = make_action("map", "Map (Select)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    act_stick_click[1] = make_action("gun_select", "Gun select (hold + flick)",
                                     XR_ACTION_TYPE_BOOLEAN_INPUT);
    act_aim_pose = make_action("aim_pose", "Aim pose", XR_ACTION_TYPE_POSE_INPUT, true);
    act_grip_pose = make_action("grip_pose", "Grip pose", XR_ACTION_TYPE_POSE_INPUT, true);
    act_haptic = make_action("vibration", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);

    // Meta Quest Touch (Quest 2/3/Pro over Link, Air Link, Virtual Desktop or SteamVR)
    bool any = suggest("/interaction_profiles/oculus/touch_controller",
                       {{act_move, "/user/hand/left/input/thumbstick"},
                        {act_look, "/user/hand/right/input/thumbstick"},
                        {act_a, "/user/hand/right/input/a/click"},
                        {act_b, "/user/hand/right/input/b/click"},
                        {act_x, "/user/hand/left/input/x/click"},
                        {act_y, "/user/hand/left/input/y/click"},
                        {act_menu, "/user/hand/left/input/menu/click"},
                        {act_trigger[0], "/user/hand/left/input/trigger/value"},
                        {act_trigger[1], "/user/hand/right/input/trigger/value"},
                        {act_grip[0], "/user/hand/left/input/squeeze/value"},
                        {act_grip[1], "/user/hand/right/input/squeeze/value"},
                        {act_stick_click[0], "/user/hand/left/input/thumbstick/click"},
                        {act_stick_click[1], "/user/hand/right/input/thumbstick/click"},
                        {act_aim_pose, "/user/hand/left/input/aim/pose"},
                        {act_aim_pose, "/user/hand/right/input/aim/pose"},
                        {act_grip_pose, "/user/hand/left/input/grip/pose"},
                        {act_grip_pose, "/user/hand/right/input/grip/pose"},
                        {act_haptic, "/user/hand/left/output/haptic"},
                        {act_haptic, "/user/hand/right/output/haptic"}});

    // Valve Index: A/B on both hands stand in for A/B/X/Y; no menu button, so pause is
    // left B + right B pressed together is not supported yet - use the keyboard/gamepad.
    any |= suggest("/interaction_profiles/valve/index_controller",
                   {{act_move, "/user/hand/left/input/thumbstick"},
                    {act_look, "/user/hand/right/input/thumbstick"},
                    {act_a, "/user/hand/right/input/a/click"},
                    {act_b, "/user/hand/right/input/b/click"},
                    {act_x, "/user/hand/left/input/a/click"},
                    {act_y, "/user/hand/left/input/b/click"},
                    {act_trigger[0], "/user/hand/left/input/trigger/value"},
                    {act_trigger[1], "/user/hand/right/input/trigger/value"},
                    {act_grip[0], "/user/hand/left/input/squeeze/value"},
                    {act_grip[1], "/user/hand/right/input/squeeze/value"},
                    {act_stick_click[0], "/user/hand/left/input/thumbstick/click"},
                    {act_stick_click[1], "/user/hand/right/input/thumbstick/click"},
                    {act_aim_pose, "/user/hand/left/input/aim/pose"},
                    {act_aim_pose, "/user/hand/right/input/aim/pose"},
                    {act_grip_pose, "/user/hand/left/input/grip/pose"},
                    {act_grip_pose, "/user/hand/right/input/grip/pose"},
                    {act_haptic, "/user/hand/left/output/haptic"},
                    {act_haptic, "/user/hand/right/output/haptic"}});
    if (!any) {
      lg::warn("[VR] No controller bindings were accepted; motion controllers disabled.");
      return false;
    }

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &action_set;
    if (!check(xrAttachSessionActionSets(session, &ai), "xrAttachSessionActionSets")) {
      return false;
    }

    for (int h = 0; h < 2; h++) {
      XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
      sci.subactionPath = hand_path[h];
      sci.poseInActionSpace.orientation.w = 1.f;
      sci.action = act_aim_pose;
      check(xrCreateActionSpace(session, &sci, &aim_space[h]), "xrCreateActionSpace(aim)");
      sci.action = act_grip_pose;
      check(xrCreateActionSpace(session, &sci, &grip_space[h]), "xrCreateActionSpace(grip)");
    }
    XrReferenceSpaceCreateInfo vs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vs.poseInReferenceSpace.orientation.w = 1.f;
    check(xrCreateReferenceSpace(session, &vs, &view_space), "xrCreateReferenceSpace(view)");

    input_ready = true;
    lg::info("[VR] Motion controllers ready.");
    return true;
  }

  bool get_bool(XrAction a) {
    if (a == XR_NULL_HANDLE) return false;
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(session, &gi, &st))) return false;
    return st.isActive && st.currentState;
  }

  float get_float(XrAction a) {
    if (a == XR_NULL_HANDLE) return 0.f;
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(session, &gi, &st))) return 0.f;
    return st.isActive ? st.currentState : 0.f;
  }

  XrVector2f get_vec2(XrAction a) {
    XrVector2f out{0, 0};
    if (a == XR_NULL_HANDLE) return out;
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_FAILED(xrGetActionStateVector2f(session, &gi, &st)) || !st.isActive) return out;
    return st.currentState;
  }

  VrPose locate(XrSpace space, XrTime t) {
    VrPose out;
    if (space == XR_NULL_HANDLE) return out;
    XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    loc.next = &vel;
    if (XR_FAILED(xrLocateSpace(space, local_space, t, &loc))) return out;
    const auto need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return out;
    out.valid = true;
    out.pos[0] = loc.pose.position.x;
    out.pos[1] = loc.pose.position.y;
    out.pos[2] = loc.pose.position.z;
    out.quat[0] = loc.pose.orientation.x;
    out.quat[1] = loc.pose.orientation.y;
    out.quat[2] = loc.pose.orientation.z;
    out.quat[3] = loc.pose.orientation.w;
    if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
      out.lin_vel[0] = vel.linearVelocity.x;
      out.lin_vel[1] = vel.linearVelocity.y;
      out.lin_vel[2] = vel.linearVelocity.z;
    }
    return out;
  }

  // Did this hand just make a fast forward jab?
  bool detect_punch(const VrPose& aim, int hand, double t) {
    if (!aim.valid) return false;
    if (t < punch_ready_at[hand]) return false;
    const float* v = aim.lin_vel;
    float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (speed < kPunchSpeed) return false;
    // controller forward = orientation applied to (0, 0, -1)
    float x = aim.quat[0], y = aim.quat[1], z = aim.quat[2], w = aim.quat[3];
    float fx = -(2.f * (x * z + w * y));
    float fy = -(2.f * (y * z - w * x));
    float fz = -(1.f - 2.f * (x * x + y * y));
    float along = (v[0] * fx + v[1] * fy + v[2] * fz) / speed;
    if (along < kPunchForwardDot) return false;
    punch_until[hand] = t + kPunchHoldSec;
    punch_ready_at[hand] = t + kPunchCooldownSec;
    return true;
  }

  void clear_input_state() {
    std::lock_guard<std::mutex> lk(input_mutex);
    pad = VrPadState();
    tracking = VrTrackingState();
  }

  void update_input(XrTime display_time) {
    if (!input_ready || state != XR_SESSION_STATE_FOCUSED) {
      // headset off, or the system menu is open: don't feed stale input to the game
      clear_input_state();
      return;
    }
    XrActiveActionSet active{action_set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session, &si))) {
      clear_input_state();
      return;
    }

    VrTrackingState tr;
    tr.head = locate(view_space, display_time);
    for (int h = 0; h < 2; h++) {
      tr.hand_aim[h] = locate(aim_space[h], display_time);
      tr.hand_grip[h] = locate(grip_space[h], display_time);
    }

    VrPadState p;
    p.active = true;
    auto press = [&](int btn, float amount = 1.f) {
      p.buttons[btn] = true;
      int pi = pressure_index(btn);
      if (pi >= 0) {
        uint8_t v = (uint8_t)std::fmin(255.f, std::fmax(0.f, amount * 255.f));
        if (v > p.pressure[pi]) p.pressure[pi] = v;
      }
    };

    if (get_bool(act_a)) press(CROSS);
    if (get_bool(act_b)) press(CIRCLE);
    if (get_bool(act_x)) press(SQUARE);
    if (get_bool(act_y)) press(TRIANGLE);
    if (get_bool(act_menu)) press(START);
    if (get_bool(act_stick_click[0])) press(SELECT);

    float lt = get_float(act_trigger[0]), rt = get_float(act_trigger[1]);
    if (lt > kTriggerOn) press(L1, lt);
    if (rt > kTriggerOn) press(R1, rt);
    float lg_ = get_float(act_grip[0]), rg = get_float(act_grip[1]);
    if (lg_ > kGripOn) press(L2, lg_);
    if (rg > kGripOn) press(R2, rg);

    XrVector2f mv = get_vec2(act_move);
    if (std::fabs(mv.x) > kStickDeadzone || std::fabs(mv.y) > kStickDeadzone) {
      p.left_stick_used = true;
      p.lx = stick_to_ps2(mv.x);
      p.ly = stick_to_ps2(-mv.y);  // PS2: up is 0
    }

    XrVector2f lk = get_vec2(act_look);
    bool gun_select = get_bool(act_stick_click[1]);
    if (gun_select) {
      // click and flick the right stick to choose a gun (D-pad)
      if (std::fabs(lk.x) > 0.5f || std::fabs(lk.y) > 0.5f) {
        if (std::fabs(lk.x) > std::fabs(lk.y)) {
          press(lk.x > 0 ? DPAD_RIGHT : DPAD_LEFT);
        } else {
          press(lk.y > 0 ? DPAD_UP : DPAD_DOWN);
        }
      }
    } else if (std::fabs(lk.x) > kStickDeadzone || std::fabs(lk.y) > kStickDeadzone) {
      p.right_stick_used = true;
      p.rx = stick_to_ps2(lk.x);
      p.ry = stick_to_ps2(-lk.y);
    }

    // motion: a fast forward jab with either hand throws a punch
    double t = now_seconds();
    for (int h = 0; h < 2; h++) {
      detect_punch(tr.hand_aim[h], h, t);
      if (t < punch_until[h]) press(CIRCLE);
    }

    float rumble_now;
    {
      std::lock_guard<std::mutex> lk2(input_mutex);
      pad = p;
      tracking = tr;
      // stop if the game hasn't refreshed the rumble recently
      rumble_now = (t - rumble_time < 0.25) ? rumble : 0.f;
    }

    // vibration
    if (act_haptic != XR_NULL_HANDLE) {
      if (rumble_now > 0.02f) {
        XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
        vib.amplitude = std::fmin(1.f, rumble_now);
        vib.duration = 60'000'000;  // 60 ms, refreshed every frame while the game rumbles
        vib.frequency = XR_FREQUENCY_UNSPECIFIED;
        for (int h = 0; h < 2; h++) {
          XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
          hi.action = act_haptic;
          hi.subactionPath = hand_path[h];
          xrApplyHapticFeedback(session, &hi, (const XrHapticBaseHeader*)&vib);
        }
        rumble_on = true;
      } else if (rumble_on) {
        for (int h = 0; h < 2; h++) {
          XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
          hi.action = act_haptic;
          hi.subactionPath = hand_path[h];
          xrStopHapticFeedback(session, &hi);
        }
        rumble_on = false;
      }
    }
  }

  void destroy_input() {
    input_ready = false;
    for (int h = 0; h < 2; h++) {
      if (aim_space[h] != XR_NULL_HANDLE) xrDestroySpace(aim_space[h]);
      if (grip_space[h] != XR_NULL_HANDLE) xrDestroySpace(grip_space[h]);
      aim_space[h] = grip_space[h] = XR_NULL_HANDLE;
    }
    if (view_space != XR_NULL_HANDLE) {
      xrDestroySpace(view_space);
      view_space = XR_NULL_HANDLE;
    }
    if (action_set != XR_NULL_HANDLE) {
      xrDestroyActionSet(action_set);  // also destroys its actions
      action_set = XR_NULL_HANDLE;
    }
    clear_input_state();
  }

  void destroy_swapchain() {
    if (swapchain != XR_NULL_HANDLE) {
      xrDestroySwapchain(swapchain);
      swapchain = XR_NULL_HANDLE;
    }
    sc_images.clear();
    sc_width = sc_height = 0;
  }

  bool ensure_swapchain(int w, int h) {
    if (swapchain != XR_NULL_HANDLE && w == sc_width && h == sc_height) {
      return true;
    }
    destroy_swapchain();

    if (swapchain_format == 0) {
      uint32_t n = 0;
      xrEnumerateSwapchainFormats(session, 0, &n, nullptr);
      std::vector<int64_t> formats(n);
      xrEnumerateSwapchainFormats(session, n, &n, formats.data());
      for (auto f : formats) {
        if (f == kGlSrgb8Alpha8) {
          swapchain_format = f;
          break;
        }
      }
      if (swapchain_format == 0) {
        for (auto f : formats) {
          if (f == kGlRgba8) {
            swapchain_format = f;
            break;
          }
        }
      }
      if (swapchain_format == 0) {
        if (formats.empty()) {
          lg::error("[VR] Runtime offered no swapchain formats.");
          return false;
        }
        swapchain_format = formats[0];
      }
      lg::info("[VR] Using swapchain format 0x{:x}", swapchain_format);
    }

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = swapchain_format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!check(xrCreateSwapchain(session, &ci, &swapchain), "xrCreateSwapchain")) {
      swapchain = XR_NULL_HANDLE;
      return false;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(swapchain, 0, &n, nullptr);
    sc_images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    xrEnumerateSwapchainImages(swapchain, n, &n, (XrSwapchainImageBaseHeader*)sc_images.data());
    sc_width = w;
    sc_height = h;
    lg::info("[VR] Created screen swapchain {}x{} ({} images)", w, h, n);
    return true;
  }

  void poll_events() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
      switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
          auto* sc = (XrEventDataSessionStateChanged*)&ev;
          state = sc->state;
          lg::info("[VR] Session state -> {}", (int)state);
          if (state == XR_SESSION_STATE_READY && !session_running) {
            XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
            bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (check(xrBeginSession(session, &bi), "xrBeginSession")) {
              session_running = true;
            }
          } else if (state == XR_SESSION_STATE_STOPPING && session_running) {
            check(xrEndSession(session), "xrEndSession");
            session_running = false;
          } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
            // The runtime is going away (e.g. Link was disconnected). Drop back to flat mode.
            lg::warn("[VR] Headset session ended. Continuing without VR.");
            teardown();
            return;
          }
          break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
          lg::warn("[VR] OpenXR runtime is shutting down. Continuing without VR.");
          teardown();
          return;
        default:
          break;
      }
      ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
  }

  void teardown() {
    destroy_input();
    destroy_swapchain();
    if (blit_fbo) {
      glDeleteFramebuffers(1, &blit_fbo);
      blit_fbo = 0;
    }
    if (local_space != XR_NULL_HANDLE) {
      xrDestroySpace(local_space);
      local_space = XR_NULL_HANDLE;
    }
    if (session != XR_NULL_HANDLE) {
      xrDestroySession(session);
      session = XR_NULL_HANDLE;
    }
    if (instance != XR_NULL_HANDLE) {
      xrDestroyInstance(instance);
      instance = XR_NULL_HANDLE;
    }
    session_running = false;
    frame_begun = false;
    initialized = false;
  }

  void copy_to_swapchain(GLuint read_fbo, int x, int y, int w, int h) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!check(xrAcquireSwapchainImage(swapchain, &ai, &idx), "xrAcquireSwapchainImage")) {
      return;
    }
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (!check(xrWaitSwapchainImage(swapchain, &wi), "xrWaitSwapchainImage")) {
      return;
    }

    // save the GL state we touch
    GLint prev_read = 0, prev_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);

    if (!blit_fbo) {
      glGenFramebuffers(1, &blit_fbo);
    }
    glDisable(GL_SCISSOR_TEST);
    // copy the bytes as-is: the game's image is already gamma-encoded, which is what an sRGB
    // swapchain expects.
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    if (read_fbo == 0) {
      glReadBuffer(GL_BACK);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blit_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           sc_images[idx].image, 0);
    glBlitFramebuffer(x, y, x + w, y + h, 0, 0, sc_width, sc_height, GL_COLOR_BUFFER_BIT,
                      GL_LINEAR);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
    if (scissor) {
      glEnable(GL_SCISSOR_TEST);
    }
    if (srgb) {
      glEnable(GL_FRAMEBUFFER_SRGB);
    }

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    check(xrReleaseSwapchainImage(swapchain, &ri), "xrReleaseSwapchainImage");
  }
};

OpenXrSession::OpenXrSession() : m_impl(std::make_unique<Impl>()) {}

OpenXrSession::~OpenXrSession() {
  shutdown();
}

bool OpenXrSession::init(void* sdl_window) {
  if (m_impl->initialized) {
    return true;
  }
  const char* disable = std::getenv("OPENGOAL_NO_VR");
  if (disable && disable[0] && disable[0] != '0') {
    lg::info("[VR] Disabled by OPENGOAL_NO_VR.");
    return false;
  }
  if (!m_impl->create_instance() || !m_impl->get_system() || !m_impl->create_session(sdl_window)) {
    m_impl->teardown();
    return false;
  }
  m_impl->create_input();  // optional: VR still works with a gamepad if this fails
  m_impl->initialized = true;
  lg::info("[VR] VR ready. Put on your headset.");
  return true;
}

void OpenXrSession::shutdown() {
  if (m_impl) {
    m_impl->teardown();
  }
}

bool OpenXrSession::is_initialized() const {
  return m_impl->initialized;
}

bool OpenXrSession::is_running() const {
  return m_impl->initialized && m_impl->session_running;
}

void OpenXrSession::begin_frame() {
  auto& s = *m_impl;
  s.frame_begun = false;
  if (!s.initialized) {
    return;
  }
  s.poll_events();
  if (!s.initialized || !s.session_running) {
    if (s.initialized) {
      s.clear_input_state();
    }
    return;
  }

  s.frame_state = {XR_TYPE_FRAME_STATE};
  XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
  if (!s.check(xrWaitFrame(s.session, &wi, &s.frame_state), "xrWaitFrame")) {
    return;
  }
  s.update_input(s.frame_state.predictedDisplayTime);
  XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
  if (!s.check(xrBeginFrame(s.session, &bi), "xrBeginFrame")) {
    return;
  }
  s.frame_begun = true;
}

void OpenXrSession::end_frame_with_screen(GLuint read_fbo, int x, int y, int w, int h) {
  auto& s = *m_impl;
  if (!s.frame_begun) {
    return;
  }
  s.frame_begun = false;

  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  const XrCompositionLayerBaseHeader* layers[1];
  uint32_t layer_count = 0;

  if (s.frame_state.shouldRender && w > 0 && h > 0) {
    // keep the swapchain a sensible size: no wider than 2560 pixels
    int sw = w, sh = h;
    if (sw > 2560) {
      sh = (int)((float)sh * 2560.f / (float)sw);
      sw = 2560;
    }
    if (s.ensure_swapchain(sw, sh)) {
      s.copy_to_swapchain(read_fbo, x, y, w, h);
      quad.layerFlags = 0;
      quad.space = s.local_space;
      quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      quad.subImage.swapchain = s.swapchain;
      quad.subImage.imageRect.offset = {0, 0};
      quad.subImage.imageRect.extent = {s.sc_width, s.sc_height};
      quad.subImage.imageArrayIndex = 0;
      quad.pose.orientation = {0.f, 0.f, 0.f, 1.f};
      quad.pose.position = {0.f, 0.f, -kScreenDistanceM};
      quad.size.width = kScreenWidthM;
      quad.size.height = kScreenWidthM * (float)h / (float)w;
      layers[layer_count++] = (const XrCompositionLayerBaseHeader*)&quad;
    }
  }

  XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
  ei.displayTime = s.frame_state.predictedDisplayTime;
  ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  ei.layerCount = layer_count;
  ei.layers = layers;
  s.check(xrEndFrame(s.session, &ei), "xrEndFrame");
}

VrPadState OpenXrSession::get_pad_state() const {
  std::lock_guard<std::mutex> lk(m_impl->input_mutex);
  return m_impl->pad;
}

VrTrackingState OpenXrSession::get_tracking_state() const {
  std::lock_guard<std::mutex> lk(m_impl->input_mutex);
  return m_impl->tracking;
}

void OpenXrSession::set_rumble(float strength) {
  std::lock_guard<std::mutex> lk(m_impl->input_mutex);
  m_impl->rumble = strength;
  m_impl->rumble_time = now_seconds();
}

OpenXrSession& get_session() {
  static OpenXrSession session;
  return session;
}

}  // namespace vr
