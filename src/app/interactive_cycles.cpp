#include "interactive_cycles.h"

#include <stdio.h>
#include <memory>

#include "device/device.h"
#include "integrator/path_trace.h"
#include "scene/camera.h"
#include "scene/integrator.h"
#include "scene/scene.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "session/buffers.h"
#include "session/session.h"
#include "session/output_driver.h"

#include "opengl/display_driver.h"
#include "opengl/window.h"


using namespace ccl;
using namespace cycles_wrapper;

namespace cycles_wrapper {

class InteractiveCyclesOutputDriver : public OutputDriver {
 public:
  InteractiveCyclesOutputDriver(InteractiveCycles *interactiveCycles)
      : OutputDriver(), mInteractiveCycles(interactiveCycles)
  {
  }

  void write_render_tile(const Tile &tile) override
  {
    mInteractiveCycles->mFrameFinished = true;
  }

 private:
  InteractiveCycles *mInteractiveCycles;
};

class InteractiveCyclesOpenGLDisplayDriver : public OpenGLDisplayDriver {

 public:
  InteractiveCyclesOpenGLDisplayDriver(InteractiveCycles *interactiveCycles,
                                       const std::function<bool()> &gl_context_enable,
                                       const std::function<void()> &gl_context_disable)
      : OpenGLDisplayDriver(gl_context_enable, gl_context_disable),
        mInteractiveCycles(interactiveCycles)
  {
  }

  virtual void draw(const Params &params) override
  {
    if (mInteractiveCycles->UseOuterContext()) {

      /* See do_update_begin() for why no locking is required here. */
      if (texture_.need_clear) {
        /* Texture is requested to be cleared and was not yet cleared.
         * Do early return which should be equivalent of drawing all-zero texture. */
        return;
      }

      if (!gl_draw_resources_ensure()) {
        return;
      }

      DisplayDriverDelegateParams dddp;
      dddp.gl_upload_sync_ = gl_upload_sync_;
      dddp.useNearestPointSampling = (texture_.width != params.size.x ||
                                      texture_.height != params.size.y);
      dddp.texId = texture_.gl_id;
      dddp.vertexBufferId = vertex_buffer_;
      mInteractiveCycles->DrawWithOuterContext_DisplayDriverDelegate(0, &dddp);
      display_shader_.bind(params.full_size.x, params.full_size.y);
      mInteractiveCycles->DrawWithOuterContext_DisplayDriverDelegate(1, &dddp);
      texture_update_if_needed();
      Params pTemp = params;
      // Account for the difference in coordinate systems
      pTemp.size.x = -params.size.x;
      pTemp.full_size.x = -params.full_size.x;
      pTemp.full_offset.x = params.full_size.x;
      vertex_buffer_update(pTemp);
      dddp.texcoordAttribute = display_shader_.get_tex_coord_attrib_location();
      dddp.positionAttribute = display_shader_.get_position_attrib_location();
      mInteractiveCycles->DrawWithOuterContext_DisplayDriverDelegate(2, &dddp);
      display_shader_.unbind();
      mInteractiveCycles->DrawWithOuterContext_DisplayDriverDelegate(3, &dddp);
      gl_render_sync_ = dddp.gl_render_sync_;
    }
    else {
      OpenGLDisplayDriver::draw(params);
    }
  }

 private:
  InteractiveCycles *mInteractiveCycles;
};
}  // namespace cycles_wrapper

InteractiveCycles::InteractiveCycles() 
	: CyclesEngine()
{
  /* device names */
  string device_names = "";
  string devicename = "CPU";

  /* List devices for which support is compiled in. */
  vector<DeviceType> types = Device::available_types();
  for (DeviceType &type : types) {
    if (device_names != "")
      device_names += ", ";

    device_names += Device::string_from_type(type);
  }

  /* parse options */
  bool profile = false, debug = false;
  int verbosity = 1;

  if (debug) {
    util_logging_start();
    util_logging_verbosity_set(verbosity);
  }

  mOptions.session_params->use_profiling = profile;
  mOptions.interactive = true;
  //mOptions.session_params->threads = 14;
  //mOptions.session_params->background = true;

  if (mOptions.session_params->tile_size > 0) {
    mOptions.session_params->use_auto_tile = true;
  }

  /* find matching device */
  DeviceType device_type = Device::type_from_string(devicename.c_str());
  vector<DeviceInfo> devices = Device::available_devices(DEVICE_MASK(device_type));
  bool device_available = false;
  if (!devices.empty()) {
    mOptions.session_params->device = devices.front();
    device_available = true;
  }

  /* handle invalid configurations */
  if (mOptions.session_params->device.type == DEVICE_NONE || !device_available) {
    fprintf(stderr, "Unknown device: %s\n", devicename.c_str());
    exit(EXIT_FAILURE);
  }
#ifdef WITH_OSL
  else if (mOptions.scene_params->shadingsystem == SHADINGSYSTEM_OSL &&
           mOptions.session_params->device.type != DEVICE_CPU) {
    fprintf(stderr, "OSL shading system only works with CPU device\n");
    exit(EXIT_FAILURE);
  }
#endif
  else if (mOptions.session_params->samples < 0) {
    fprintf(stderr, "Invalid number of samples: %d\n", mOptions.session_params->samples);
    exit(EXIT_FAILURE);
  }
}

InteractiveCycles::~InteractiveCycles()
{
  if (mOptions.session.get() != nullptr) {
    SessionExit();
  }

  if (IsOpenGLInitialized()) {
    DeinitializeOpenGL();
  }
}

void InteractiveCycles::SessionPrintStatus()
{
  if (!mOptions.session)
    return;

  int sample = (mFrameFinished) ? mOptions.session->params.samples :
                                  mOptions.session->progress.get_current_sample();

  string status, substatus;
  mOptions.session->progress.get_status(status, substatus);

  if (status._Starts_with("Sample ")) {
    // reconstruct the string
    status = "INTERACTIVE_CYCLES_PROGRESS: " + std::to_string(sample) + "/" +
             std::to_string(mOptions.session->params.samples);
    Log(LOG_TYPE_INFO, status);

    // set the status
    status = "INTERACTIVE_CYCLES_STATUS: Pathtracing...";
    Log(LOG_TYPE_INFO, status);
  }
  else {
    if (status._Starts_with("Rendering Done"))
      status = "Ready";
    else if (substatus != "")
      status += ": " + substatus;

    /* print status */
    status = string_printf("INTERACTIVE_CYCLES_STATUS: %s", status.c_str());
    Log(LOG_TYPE_INFO, status);
  }
}

void InteractiveCycles::ResetSession()
{
  CyclesEngine::ResetSession();
  mFrameFinished = false;
}

void InteractiveCycles::MouseUpdate(float x, float y, bool left, bool right, int wheel_delta)
{
  x -= mOptions.width / 2;
  y -= mOptions.height / 2;
  mMouse.LastX = x;
  mMouse.LastY = y;
  mMouse.PrevLeft = left;
  mMouse.PrevRight = right;
  mMouse.WheelDelta = wheel_delta;
}

void InteractiveCycles::SetSuspended(bool isSuspended)
{
  mSuspended = isSuspended;
  if (isSuspended) {
    CancelSession();
  }
  else { // !isSuspended
    PostSceneUpdate();
  }
}

void InteractiveCycles::Draw()
{
  if (mSuspended)
    return;

  if (mUseOuterContext) {
    // The outer context is already set outside
    mOptions.session->draw();
  }
  else {
    EnableContextOpenGL();
    SetViewportOpenGL();
    mOptions.session->draw();
    DisableContextOpenGL();
  }
}

bool InteractiveCycles::SessionInit()
{
  CyclesEngine::SessionInit();
  mOptions.output_pass = "combined";
  mOptions.session = std::make_unique<Session>(*mOptions.session_params, *mOptions.scene_params);

  //mOptions.session->scene->integrator->set_use_denoise(true);
  //mOptions.session->scene->integrator->set_start_sample(200);
  //mOptions.session->scene->integrator->tag_modified();

  if (!mOptions.session_params->background) {

    std::unique_ptr<OpenGLDisplayDriver> dd =
        std::make_unique<InteractiveCyclesOpenGLDisplayDriver>(
            this,
            [this] { return ACQUIRE_LOCK this->EnableContextOpenGL(); },
            [this] { RELEASE_LOCK this->DisableContextOpenGL(); });

    mDriverPtr = (InteractiveCyclesOpenGLDisplayDriver *)dd.get();
    mOptions.session->set_display_driver(std::move(dd));
  }


  if (!mOptions.quiet) {
    mOptions.session->set_output_driver(make_unique<InteractiveCyclesOutputDriver>(this));
    mOptions.session->progress.set_update_callback([this]() { this->SessionPrintStatus(); });
  }

  DefaultSceneInit();
  PostSceneUpdate();
  SetSuspended(mSuspended); // restore the suspended state

  return true;
}

bool InteractiveCycles::SessionExit()
{
  CyclesEngine::SessionExit();
  mOptions.session.reset();

  if (mOptions.session_params->background && !mOptions.quiet) {
    Log(LOG_TYPE_INFO, "Finished Rendering.");
    printf("\n");
  }
  return true;
}
