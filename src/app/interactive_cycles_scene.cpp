#include "interactive_cycles.h"
#include "image_memory_oiio.h"

#include <stdio.h>
#include <memory>

#include "device/device.h"
#include "scene/camera.h"
#include "scene/integrator.h"
#include "scene/osl.h"
#include "scene/svm.h"
#include "scene/procedural.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "session/buffers.h"
#include "session/session.h"
#include "app/cycles_xml.h"


using namespace ccl;
using namespace cycles_wrapper;

const std::string gLightShaderName = "qi_shader_light";
const std::string gTexturedShaderName = "qi_shader_textured";

void InteractiveCycles::DefaultSceneInit()
{
  CyclesEngine::DefaultSceneInit();
}

void InteractiveCycles::PostSceneUpdate()
{
  CyclesEngine::PostSceneUpdate();



  // Update camera
  ccl::Scene *scene = mOptions.session->scene;
  scene->camera->set_matrix(*mCameraTransform);
  switch (mCameraType) {
    case Perspective:
      scene->camera->set_camera_type(CAMERA_PERSPECTIVE);
      break;
    case Orthographic:
      scene->camera->set_camera_type(CAMERA_ORTHOGRAPHIC);
      break;
    case Panoramic:
      scene->camera->set_camera_type(CAMERA_PANORAMA);
      scene->camera->set_panorama_type(PANORAMA_EQUIRECTANGULAR);
      break;
    default:
      break;
  }
  scene->camera->set_full_width(mOptions.width);
  scene->camera->set_full_height(mOptions.height);
  scene->camera->compute_auto_viewplane();
  scene->camera->need_flags_update = true;
  scene->camera->need_device_update = true;

  // Start the session
  ResetSession();
  mOptions.session->start();
}
