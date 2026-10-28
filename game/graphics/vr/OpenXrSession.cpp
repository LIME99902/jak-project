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

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    return;
  }

  s.frame_state = {XR_TYPE_FRAME_STATE};
  XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
  if (!s.check(xrWaitFrame(s.session, &wi, &s.frame_state), "xrWaitFrame")) {
    return;
  }
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

OpenXrSession& get_session() {
  static OpenXrSession session;
  return session;
}

}  // namespace vr
