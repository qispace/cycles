#include "cycles_engine.h"

#include <stdio.h>
#include <memory>

#include "device/device.h"
#include "scene/camera.h"
#include "scene/integrator.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "session/buffers.h"
#include "session/session.h"
#include "app/cycles_xml.h"

#include "opengl/display_driver.h"
#include "opengl/window.h"


using namespace ccl;
using namespace cycles_wrapper;

CyclesEngine::CyclesEngine()
{
  mCameraTransform = std::make_unique<ccl::Transform>();
  mOptions.scene_params = std::make_unique<SceneParams>();
  mOptions.session_params = std::make_unique<SessionParams>();
#ifdef DEBUG
  mOptions.width = 128;
  mOptions.height = 64;
#else
  mOptions.width = 1024;
  mOptions.height = 512;
#endif
  mOptions.session = NULL;
  mOptions.quiet = false;
  mOptions.show_help = false;
  mOptions.interactive = false;
  mOptions.pause = false;
  mOptions.output_pass = "";
  mOptions.session_params->use_auto_tile = false;
  mOptions.session_params->tile_size = 0;

  mViewportWidth = mOptions.width;
  mViewportHeight = mOptions.height;

  // No need for any kind of crazy shading for now. If, however, OSL
  // is used then there needs to be a 'shader' folder next to the executable
  // with 'stdcycles.h' and 'stdosl.h' inside. Also all compiled shaders (.oso files)
  // from the build directory of cycles (made by CMake) need to be copied into this folder.
  string ssname = "svm"; 
  if (ssname == "osl")
    mOptions.scene_params->shadingsystem = SHADINGSYSTEM_OSL;
  else if (ssname == "svm")
    mOptions.scene_params->shadingsystem = SHADINGSYSTEM_SVM;
}

CyclesEngine::~CyclesEngine()
{

}

bool CyclesEngine::SessionInit()
{
  return true;
}

bool CyclesEngine::SessionExit()
{
  mImageHandles.clear();
  return true;
}

void CyclesEngine::PostSceneUpdate()
{

}

void CyclesEngine::SetLogFunction(void* logObject, LogFunctionPtr logFunction)
{
  mLogObjectPtr = logObject;
  mLogFunctionPtr = logFunction;
}

void CyclesEngine::Log(int type, const std::string &msg)
{
  mLogFunctionPtr(mLogObjectPtr, type, msg.c_str());
}

BufferParams CyclesEngine::GetBufferParams()
{
  BufferParams buffer_params;
  buffer_params.width = mOptions.width;
  buffer_params.height = mOptions.height;
  buffer_params.full_width = mOptions.width;
  buffer_params.full_height = mOptions.height;
  return buffer_params;
}

void CyclesEngine::ResetSession()
{
  mOptions.session->reset(*mOptions.session_params, GetBufferParams());
  mOptions.session->progress.reset();
  mCurrentSample = -1;
}

void CyclesEngine::CancelSession()
{
  mOptions.session->cancel(true);
}

int CyclesEngine::GetViewportWidth()
{
  return mViewportWidth;
}

int CyclesEngine::GetViewportHeight()
{
  return mViewportHeight;
}

void CyclesEngine::Resize(unsigned int width, unsigned int height)
{
  mOptions.width = mViewportWidth = width;
  mOptions.height = mViewportHeight = height;

  if (mOptions.session) {
    ccl::Scene *scene = mOptions.session->scene;
    if (scene) {
      scene->camera->set_full_width(mOptions.width);
      scene->camera->set_full_height(mOptions.height);
      scene->camera->compute_auto_viewplane();
      scene->camera->need_flags_update = true;
      scene->camera->need_device_update = true;
    }

    ResetSession();
  }
}

void CyclesEngine::SetCamera(CameraType cameraType,
                             float p[],
                             float d[],
                             float u[],
                             float fov,
                             float n,
                             float f)
{
  float3 pos = make_float3(p[0], p[1], p[2]);
  float3 dir = make_float3(d[0], d[1], d[2]);
  dir = normalize(dir);
  float3 up = make_float3(u[0], u[1], u[2]);
  float3 right = cross(up, dir);
  right = normalize(right);
  up = cross(dir, right);
  up = normalize(up);

  transform_set_column(mCameraTransform.get(), 0, right);
  transform_set_column(mCameraTransform.get(), 1, up);
  transform_set_column(mCameraTransform.get(), 2, dir);
  transform_set_column(mCameraTransform.get(), 3, pos);
  auto camera = mOptions.session->scene->camera;
  camera->set_matrix(*mCameraTransform);

  // Clipping
  //camera->set_farclip(FLT_MAX);
  camera->set_nearclip(n);
  camera->set_farclip(f);

  // Type
  mCameraType = cameraType;
  switch (mCameraType) {
    case Perspective:
      camera->set_camera_type(CAMERA_PERSPECTIVE);
      camera->set_fov(fov);
      break;
    case Orthographic:
      camera->set_camera_type(CAMERA_ORTHOGRAPHIC);
      break;
    case Panoramic:
      camera->set_camera_type(CAMERA_PANORAMA);
      camera->set_panorama_type(PANORAMA_EQUIRECTANGULAR);
      break;
    default:
      break;
  }

  // Update and Reset
  camera->compute_auto_viewplane();
  camera->need_flags_update = true;
  camera->need_device_update = true;
  ResetSession();
}

void CyclesEngine::GetCamera(float p[], float d[], float u[], float *n, float *f, float *fov, float *aspect)
{
  Camera *camera = mOptions.session->scene->camera;
  *n = camera->get_nearclip();
  *f = camera->get_farclip();
  *fov = camera->get_fov();
  *aspect = (float)camera->get_full_width() / camera->get_full_height();
  Transform matrix = camera->get_matrix();

  // Position
  for (int rows = 0; rows < 3; rows++) {
    p[rows] = matrix[rows][3];
  }

  // Direction
  for (int rows = 0; rows < 3; rows++) {
    d[rows] = matrix[rows][2];
  }

  // Up
  for (int rows = 0; rows < 3; rows++) {
    u[rows] = matrix[rows][1];
  }
}

void CyclesEngine::SetDenoising(const DenoisingOptions &options)
{
  mOptions.session->scene->integrator->set_use_denoise(options.mEnable);
  ccl::DenoiserPrefilter prefilter;
  if (options.mPrefilter)
  {
      prefilter = ccl::DenoiserPrefilter::DENOISER_PREFILTER_ACCURATE;
  }
  else
  {
      prefilter = ccl::DenoiserPrefilter::DENOISER_PREFILTER_NONE;
  }
  mOptions.session->scene->integrator->set_denoiser_prefilter(prefilter);
  mOptions.session->scene->integrator->tag_modified();
}
