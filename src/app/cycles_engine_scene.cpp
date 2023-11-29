#include "cycles_engine.h"
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
#include "scene/light.h"
#include "scene/background.h"
#include "session/buffers.h"
#include "session/session.h"
#include "app/cycles_xml.h"


using namespace cycles_wrapper;

const std::string CyclesEngine::sDefaultSurfaceShaderName = "qi_shader_default_surface";
const std::string CyclesEngine::sLightShaderName = "qi_shader_light";
const std::string CyclesEngine::sDisabledLightShaderName = "qi_shader_light_disabled";
const std::string CyclesEngine::sColorBackgroundShaderName = "qi_shader_background_color";
const std::string CyclesEngine::sSkyBackgroundShaderName = "qi_shader_background_sky";

void CyclesEngine::DefaultSceneInit()
{
  // Set up shaders
  ccl::Scene *scene = mOptions.session->scene;
  mNameToShader.clear();
  // Surface
  {
    ccl::ShaderGraph *graph = new ccl::ShaderGraph();

    // Ablbedo
    ccl::float3 f3AlbedoColor = ccl::make_float3(1.0f, 1.0f, 1.0f);
    ccl::ColorNode *albedoColorNode = graph->create_node<ccl::ColorNode>();
    albedoColorNode->set_value(f3AlbedoColor);
    graph->add(albedoColorNode);
    ccl::ShaderOutput *albedoOutput = albedoColorNode->output("Color");

    // BSDF
    ccl::PrincipledBsdfNode *bsdfNode = graph->create_node<ccl::PrincipledBsdfNode>();
    //bsdfNode->set_metallic(1.0f);
    //bsdfNode->set_roughness(0.1f);
    graph->add(bsdfNode);
    ccl::ShaderOutput *bsdfOutput = bsdfNode->output("BSDF");

    // Final connections
    graph->connect(albedoOutput, bsdfNode->input("Base Color"));
    graph->connect(bsdfOutput, graph->output()->input("Surface"));

    ccl::Shader *shader = scene->create_node<ccl::Shader>();
    shader->name = sDefaultSurfaceShaderName;
    shader->set_graph(graph);
    shader->reference();
    shader->tag_update(scene);
    mNameToShader[shader->name.c_str()] = shader;
  }
  // Light
  {
    ccl::ShaderGraph *graph = new ccl::ShaderGraph();
    ccl::EmissionNode *emission = graph->create_node<ccl::EmissionNode>();
    emission->set_color(ccl::make_float3(1.0f, 1.0f, 1.0f));
    emission->set_strength(1.0f);
    graph->add(emission);

    graph->connect(emission->output("Emission"), graph->output()->input("Surface"));

    ccl::Shader *shader = scene->create_node<ccl::Shader>();
    shader->name = sLightShaderName;
    shader->set_graph(graph);
    shader->reference();
    shader->tag_update(scene);
    mNameToShader[shader->name.c_str()] = shader;
  }
  // Light (disabled)
  {
    ccl::ShaderGraph *graph = new ccl::ShaderGraph();
    ccl::EmissionNode *emission = graph->create_node<ccl::EmissionNode>();
    emission->set_color(ccl::make_float3(1.0f, 1.0f, 1.0f));
    emission->set_strength(0.0f); // this makes it disabled
    graph->add(emission);

    graph->connect(emission->output("Emission"), graph->output()->input("Surface"));

    ccl::Shader *shader = scene->create_node<ccl::Shader>();
    shader->name = sDisabledLightShaderName;
    shader->set_graph(graph);
    shader->reference();
    shader->tag_update(scene);
    mNameToShader[shader->name.c_str()] = shader;
  }
  // Color Background (set as default background)
  {
    ccl::ShaderGraph *graph = new ccl::ShaderGraph();

    ccl::ColorNode *colorNode = graph->create_node<ccl::ColorNode>();
    colorNode->set_value(ccl::make_float3(0.0f));
    graph->add(colorNode);

    graph->connect(colorNode->output("Color"), graph->output()->input("Surface"));

    ccl::Shader *shader = scene->create_node<ccl::Shader>();
    shader->name = sColorBackgroundShaderName;
    shader->set_graph(graph);
    shader->reference();
    shader->tag_update(scene);
    graph->simplified = true; // prevent further simplification and node removal
    mNameToShader[shader->name.c_str()] = shader;
    scene->default_background = shader;
    mCurrentBackgroundShaderName = shader->name.c_str();
  }

  // Sky Background
  {
    ccl::ShaderGraph *graph = new ccl::ShaderGraph();

    ccl::TextureCoordinateNode *texCoordNode = graph->create_node<ccl::TextureCoordinateNode>();
    graph->add(texCoordNode);

    ccl::MappingNode *mappingNode = graph->create_node<ccl::MappingNode>();
    // Account for the positive z being up, in blender and cycles
    mappingNode->set_rotation(ccl::make_float3(M_PI_2, 0.0f, 0.0f));
    graph->add(mappingNode);

    ccl::SkyTextureNode *skyNode = graph->create_node<ccl::SkyTextureNode>();
    skyNode->set_altitude(0.0f);
    skyNode->set_sun_disc(false);
    skyNode->set_sun_size(0.0095f);
    skyNode->set_sky_type(ccl::NodeSkyType::NODE_SKY_NISHITA);
    skyNode->set_air_density(1.0f);
    skyNode->set_dust_density(0.3f);
    skyNode->set_ozone_density(1.0f);
    skyNode->set_sun_elevation(M_PI_F / 16.0f);
    skyNode->set_sun_rotation(M_PI_2_F);
    graph->add(skyNode);

    graph->connect(texCoordNode->output("Generated"), mappingNode->input("Vector"));
    graph->connect(mappingNode->output("Vector"), skyNode->input("Vector"));
    graph->connect(skyNode->output("Color"), graph->output()->input("Surface"));

    ccl::Shader *shader = scene->create_node<ccl::Shader>();
    shader->name = sSkyBackgroundShaderName;
    shader->set_graph(graph);
    shader->reference();
    shader->tag_update(scene);
    graph->simplified = true; // prevent further simplification and node removal
    mNameToShader[shader->name.c_str()] = shader;
    //scene->default_background = shader;
    //mCurrentBackgroundShaderName = shader->name.c_str();
  }
}

Scene *CyclesEngine::GetScene()
{
  return (cycles_wrapper::Scene*)((mOptions.session) ? mOptions.session->scene : nullptr);
}

void CyclesEngine::CleanScene(Scene *scene)
{
  if (scene == nullptr)
    return;

  // Remove unused objects
  std::set<const ccl::Transform *> transformsToKeep;
  std::set<const ccl::Mesh *> meshesToKeep;
  std::set<const ccl::Shader *> shadersToKeep;
  ccl::Scene *s = (ccl::Scene *)scene;
  for (size_t i = 0; i < s->objects.size(); i++) {  // go over objects
    ccl::Object *obj = s->objects[i];
    transformsToKeep.insert(&obj->get_tfm());
    ccl::Mesh *mesh = dynamic_cast<ccl::Mesh *>(obj->get_geometry());
    meshesToKeep.insert(mesh);
    ccl::array<ccl::Node *> used_shaders = mesh->get_used_shaders();
    for (size_t j = 0; j < used_shaders.size(); j++) {
      ccl::Shader *shader = dynamic_cast<ccl::Shader *>(used_shaders[j]);
      shadersToKeep.insert(shader);
    }
  }
  for (size_t i = 0; i < s->lights.size(); i++) {  // go over lights
    ccl::Light *light = s->lights[i];
    transformsToKeep.insert(&light->get_tfm());
  }

  // Remove the materials from the vector
  std::set<const ccl::ImageHandle *> imagesToKeep;
  auto matVectorIte = std::begin(mMaterials);
  while (matVectorIte != std::end(mMaterials)) {
    if (shadersToKeep.find((*matVectorIte)->pbrShader) == shadersToKeep.end() &&
        shadersToKeep.find((*matVectorIte)->depthShader) == shadersToKeep.end() &&
        shadersToKeep.find((*matVectorIte)->normalShader) == shadersToKeep.end() &&
        shadersToKeep.find((*matVectorIte)->albedoShader) == shadersToKeep.end()) {
      s->delete_node(matVectorIte->get()->pbrShader);
      s->delete_node(matVectorIte->get()->depthShader);
      s->delete_node(matVectorIte->get()->normalShader);
      s->delete_node(matVectorIte->get()->albedoShader);
      matVectorIte = mMaterials.erase(matVectorIte);
    }
    else  // keep
    {
      for (auto imageHandle : matVectorIte->get()->usedImages) {
        imagesToKeep.insert(imageHandle);
      }
      ++matVectorIte;
    }
  }

  // Remove the images from the vector
  auto imageVectorIte = std::begin(mImageHandles);
  while (imageVectorIte != std::end(mImageHandles)) {
    if (imagesToKeep.find(imageVectorIte->get()) == imagesToKeep.end()) {
      imageVectorIte->get()->clear();
      imageVectorIte = mImageHandles.erase(imageVectorIte);
    }
    else  // keep
    {
      ++imageVectorIte;
    }
  }
  ccl::ImageManager *image_manager = s->image_manager;
  image_manager->tag_update();

  // Remove the nodes from the vector
  auto nodeVectorIte = std::begin(mNodes);
  while (nodeVectorIte != std::end(mNodes)) {
    if (transformsToKeep.find((*nodeVectorIte)->transform.get()) == transformsToKeep.end()) {
      nodeVectorIte = mNodes.erase(nodeVectorIte);
    }
    else  // keep
    {
      ++nodeVectorIte;
    }
  }

  // Remove all deleted nodes from the dictionary
  auto nodeMapIte = std::begin(mQiIDToNode);
  while (nodeMapIte != std::end(mQiIDToNode)) {
    if (transformsToKeep.find(nodeMapIte->second->transform.get()) == transformsToKeep.end()) {
      nodeMapIte = mQiIDToNode.erase(nodeMapIte);
    }
    else  // keep
    {
      ++nodeMapIte;
    }
  }
}

void CyclesEngine::ClearScene(Scene *scene)
{
  if (scene == nullptr)
    return;

  SessionExit();
  SessionInit();
}

void CyclesEngine::SetSceneMaxDepth(float maxDepth)
{
  mMaxDepth = maxDepth;
}

 void CyclesEngine::SetSceneBackground(const BackgroundSettings &bs)
{
  ccl::Scene *scene = mOptions.session->scene;
  ccl::Shader *oldShader = mNameToShader[mCurrentBackgroundShaderName];
  ccl::Shader *shader = nullptr;
  switch (bs.mType) {
    case BackgroundSettings::Type::Color: {
      mCurrentBackgroundShaderName = sColorBackgroundShaderName;
      mOptions.session->scene->default_background = mNameToShader[mCurrentBackgroundShaderName];
      shader = mOptions.session->scene->default_background;
      ccl::ColorNode *colorNode = nullptr;
      for (auto &it : shader->graph->nodes) {
        colorNode = dynamic_cast<ccl::ColorNode *>(it);
        if (colorNode)
          break;
      }
      if (colorNode) {
        colorNode->set_value(ccl::make_float3(bs.mColor[0], bs.mColor[1], bs.mColor[2]));
        shader->tag_update(scene);
        shader->tag_modified();
      }
      else {
        Log(LOG_TYPE_WARNING, "Failed updating the background shader. BackgroundNode not found.");
      }
    } break;

    case BackgroundSettings::Type::Sky: {
      mCurrentBackgroundShaderName = sSkyBackgroundShaderName;
      mOptions.session->scene->default_background = mNameToShader[mCurrentBackgroundShaderName];
      shader = mOptions.session->scene->default_background;
      ccl::SkyTextureNode *skyNode = nullptr;
      for (auto &it : shader->graph->nodes) {
        skyNode = dynamic_cast<ccl::SkyTextureNode *>(it);
        if (skyNode)
          break;
      }
      if (skyNode) {
        float sunElevation = M_PI_F / 16.0f;
        auto sunDir = ccl::make_float3(
            bs.mSky.mSunDirection[0], bs.mSky.mSunDirection[1], bs.mSky.mSunDirection[2]);
        if (ccl::len_squared(sunDir) > 0.0f) {
          sunDir = ccl::normalize(sunDir);
          sunElevation = M_PI_2_F - ccl::precise_angle(sunDir, ccl::make_float3(0.0f, 1.0f, 0.0f));
        }
        float sunRotation = M_PI_2_F;
        auto sunDirFlat = ccl::make_float3(
            bs.mSky.mSunDirection[0], 0.0f, bs.mSky.mSunDirection[2]);
        if (ccl::len_squared(sunDirFlat) > 0.0f) {
          sunDirFlat = ccl::normalize(sunDirFlat);
          float angleSign = ccl::dot(sunDirFlat, ccl::make_float3(1.0f, 0.0f, 0.0f));
          angleSign = (angleSign >= 0.0f) ? 1.0f : -1.0f;
          sunRotation = angleSign *
                        ccl::precise_angle(sunDirFlat, ccl::make_float3(0.0f, 0.0f, -1.0f));
          while (sunRotation < 0.0f)
            sunRotation += M_PI_F * 2.0f;
        }

        skyNode->set_sun_elevation(sunElevation);
        skyNode->set_sun_rotation(sunRotation);
        skyNode->handle.clear(); // clear the texture so that it can be recomputed
        scene->image_manager->tag_update();

        shader->tag_update(scene);
        shader->tag_modified();
      }
    } break;

    default:
      shader = oldShader;
      break;
  }

  if (oldShader != shader)
  {
    shader->tag_used(scene);
    scene->background->set_shader(shader);
    scene->background->tag_modified();
    scene->background->tag_update(scene);

    scene->background->tag_shader_modified();
  }
}

float QuatToNorm(const ccl::float4 &q)
{
  return sqrt(
      q.x * q.x +
      q.y * q.y +
      q.z * q.z +
      q.w * q.w);
}

void QuatToAxisAngle(ccl::float4 q, ccl::float3 *axis, float *angle)
{
  if (q.w > 1.0f) {
    float norm = QuatToNorm(q);
    q = ccl::make_float4(q.x / norm, q.y / norm, q.z / norm, q.w / norm);
  }

  *angle = 2.0f * acos(q.w);
  double s = sqrt(1 - q.w * q.w);  // assuming quaternion normalised then w is less than 1, so term always positive.

  if (s <= 0.0001) {
    // if s close to zero then direction of axis not important
    // if it is important that axis is normalised then replace with x=1; y=z=0;
    axis->x = 1.0f;  
    axis->y = 0.0f;
    axis->z = 0.0f;
  }
  else {
    axis->x = q.x / s;  // normalise axis
    axis->y = q.y / s;
    axis->z = q.z / s;
  }
}

Node *CyclesEngine::AddNode(Scene *scene,
                            const std::string &name,
                            Node *parent,
                            QiObjectID qiId,
                            float t[3],
                            float r[4],
                            float s[3])
{
  mNodes.push_back(std::make_unique<Node>());
  Node *node = mNodes.back().get();
  node->scene = scene;
  node->name = name;
  node->parent = parent;
  node->transform = std::make_unique<ccl::Transform>(ccl::transform_identity());
  ccl::Transform *transform = node->transform.get();
  *transform = ccl::transform_scale(s[0], s[1], s[2]) * (*transform);
  ccl::float4 quaternion = ccl::make_float4(r[0], r[1], r[2], r[3]);
  ccl::float3 axis;
  float angle;
  QuatToAxisAngle(quaternion, &axis, &angle);
  *transform = ccl::transform_rotate(angle, axis) * (*transform);
  *transform = ccl::transform_translate(t[0], t[1], t[2]) * (*transform);

  if (parent) {
    ccl::Transform *parentTransform = parent->transform.get();
    *transform = (*parentTransform) * (*transform);
    parent->children.push_back(node);
  }

  memcpy_s(node->t, sizeof(float) * 3, t, sizeof(float) * 3);
  memcpy_s(node->s, sizeof(float) * 3, s, sizeof(float) * 3);
  memcpy_s(node->r, sizeof(float) * 4, r, sizeof(float) * 4);

  mQiIDToNode[qiId] = node;
  return node;
}

Node *CyclesEngine::GetNode(QiObjectID qiId)
{
  if (mQiIDToNode.find(qiId) == mQiIDToNode.end()) {
    return nullptr;
  }
  else {
    return mQiIDToNode[qiId];
  }
}

void CyclesEngine::RemoveNode(Node *node)
{
  if (node == nullptr)
    return;

  // Remove mesh
  ccl::Scene *s = (ccl::Scene *)node->scene;
  if (node->assignedMeshObject)
    s->delete_node(node->assignedMeshObject);

  // Remove lights
  for (size_t i = 0; i < node->assignedLightObjects.size(); i++) {
    if (node->assignedLightObjects[i])
      s->delete_node(node->assignedLightObjects[i]);
  }
  node->assignedLightObjects.clear();

  // Remove the node from the parent
  if (node->parent) {
    auto &parentArray = node->parent->children;
    auto parentIte = std::find_if(
        parentArray.begin(), parentArray.end(), [&node](const Node *ptr) { return ptr == node; });
    parentArray.erase(parentIte);
  }

  // Remove the children
  for (size_t i = 0; i < node->children.size(); i++) {
    Node *childNode = node->children[i];
    // Set to NULL to prevent it from removing itself from the parent's 'children' list
    childNode->parent = nullptr; 
    RemoveNode(childNode);
  }
}

void CyclesEngine::UpdateNodeTransform(Node *node, float t[3], float r[4], float s[3])
{
  ccl::Scene *scene = (ccl::Scene *)node->scene;
  ccl::Transform *transform = node->transform.get();
  *transform = ccl::transform_identity();
  *transform = ccl::transform_scale(s[0], s[1], s[2]) * (*transform);
  ccl::float4 quaternion = ccl::make_float4(r[0], r[1], r[2], r[3]);
  ccl::float3 axis;
  float angle;
  QuatToAxisAngle(quaternion, &axis, &angle);
  *transform = ccl::transform_rotate(angle, axis) * (*transform);
  *transform = ccl::transform_translate(t[0], t[1], t[2]) * (*transform);

  if (node->parent) {
    ccl::Transform *parentTransform = node->parent->transform.get();
    *transform = (*parentTransform) * (*transform);
  }

  if (node->assignedMeshObject) {
    node->assignedMeshObject->set_tfm(*transform);
    node->assignedMeshObject->tag_update(scene);
  }

  for (size_t i = 0; i < node->assignedLightObjects.size(); i++) {
    ccl::Light *light = node->assignedLightObjects[i];
    if (light != nullptr) {
      ccl::float3 dir = ccl::make_float3(0.0f, 0.0f, -1.0f);
      dir = ccl::transform_direction(transform, dir);
      ccl::float3 pos = ccl::make_float3(0.0f);
      pos = ccl::transform_point(transform, pos);
      ccl::float3 axisu = ccl::cross(dir, ccl::make_float3(0.0f, -1.0f, 0.0f));
      axisu = ccl::normalize(axisu);
      ccl::float3 axisv = ccl::cross(dir, axisu);
      axisv = ccl::normalize(axisv);
      light->set_dir(dir);
      light->set_co(pos);
      light->set_axisu(axisu);
      light->set_axisv(axisv);
      light->set_tfm(*transform);
      light->tag_update(scene);
    }
  }

  memcpy_s(node->t, sizeof(float) * 3, t, sizeof(float) * 3);
  memcpy_s(node->s, sizeof(float) * 3, s, sizeof(float) * 3);
  memcpy_s(node->r, sizeof(float) * 4, r, sizeof(float) * 4);

  // Update the children now as well
  for (size_t i = 0; i < node->children.size(); i++) {
    Node *childNode = node->children[i];
    UpdateNodeTransform(childNode, childNode->t, childNode->r, childNode->s);
  }
}

void CyclesEngine::UpdateNodeVisibility(Node *node, bool visible)
{
  ccl::Scene *scene = (ccl::Scene *)node->scene;

  if (node->parent) {
    // ccl::Transform *parentTransform = node->parent->transform.get();
    //*transform = (*parentTransform) * (*transform);
  }

  if (node->assignedMeshObject) {
    node->assignedMeshObject->set_visibility(visible ? ~0 : 0);
    node->assignedMeshObject->tag_update(scene);
  }

  for (size_t i = 0; i < node->assignedLightObjects.size(); i++) {
    ccl::Light *light = node->assignedLightObjects[i];
    if (light) {

      ccl::Shader *shader = (visible) ? mNameToShader[sLightShaderName] :
                                        mNameToShader[sDisabledLightShaderName];
      light->set_shader(shader);
      light->tag_update(scene);
    }
  }

  node->visible = visible;

  // Update the children now as well
  for (size_t i = 0; i < node->children.size(); i++) {
    Node *childNode = node->children[i];
    UpdateNodeVisibility(childNode, visible);
  }
}

Texture *CyclesEngine::AddTexture(Scene *scene,
                                  const char *name,
                                  const unsigned char *data,
                                  size_t dataSize,
                                  const char *mimeType,
                                  bool isSRGB)
{
  if (data == nullptr)
    return nullptr;

  ccl::Scene *s = (ccl::Scene *)scene;
  ccl::ImageManager *image_manager = s->image_manager;
  ccl::ImageParams params;
  params.animated = false;
  params.interpolation = ccl::INTERPOLATION_LINEAR;
  params.extension = ccl::EXTENSION_REPEAT;
  params.alpha_type = ccl::IMAGE_ALPHA_AUTO;
  params.colorspace = "__builtin_raw";

  ccl::ImageDataType type = ccl::ImageDataType::IMAGE_DATA_TYPE_BYTE4;

  mImageHandles.push_back(std::make_unique<ccl::ImageHandle>());
  ccl::ImageHandle *ih = mImageHandles.back().get();
  *ih = image_manager->add_image(
      new OIIOImageMemoryLoader(name, data, dataSize, mimeType, isSRGB), params, false);
  image_manager->tag_update();

  return (cycles_wrapper::Texture *)ih;
}

void SetTextureTransform(ccl::ImageTextureNode *itn, const TextureTransform &tt)
{
  auto S = ccl::transform_scale(tt.scale[0], tt.scale[1], 1.0f);
  auto R = ccl::transform_rotate(tt.rotation, ccl::make_float3(0.0f, 0.0f, 1.0f));
  auto T = ccl::transform_translate(tt.offset[0], 1.0f - tt.offset[1] - tt.scale[1], 0.0f);
  auto P = ccl::transform_translate(0, -tt.scale[1], 0.0f); // rotation pivot change matrix
  auto invP = ccl::transform_inverse(P);

  auto M = T * invP * R * P * S;
  auto invM = ccl::transform_inverse(M);

  // Apply these transforms
  auto translate = ccl::transform_point(&invM, ccl::make_float3(0.0f, 0.0f, 0.0f));
  auto scale = ccl::make_float3(1.0f / tt.scale[0], 1.0f / tt.scale[1], 1.0f);
  auto rotation = ccl::make_float3(0.0f, 0.0f, -tt.rotation);

  itn->set_tex_mapping_translation(translate);
  itn->set_tex_mapping_rotation(rotation);
  itn->set_tex_mapping_scale(scale);
}

Material *CyclesEngine::AddMaterial(Scene *scene,
                                    const char *name,
                                    Texture *albedoTex,
                                    const TextureTransform &albedoTransform,
                                    float *albedoColor,
                                    Texture *metallicRoughnessTexture,
                                    const TextureTransform &metallicRoughnessTransform,
                                    float metallicFactor,
                                    float roughnessFactor,
                                    Texture *normalTex,
                                    const TextureTransform &normalTransform,
                                    float normalStrength,
                                    Texture *emissiveTex,
                                    const TextureTransform &emissiveTransform,
                                    float *emissiveFactor,
                                    float emissiveStrength,
                                    float transmissionFactor,
                                    float IOR,
                                    float *volumeAttenuationColor,
                                    float volumeThicknessFactor,
                                    float volumeAttenuationDistance)
{
  // Get scene and create a new material
  ccl::Scene *s = (ccl::Scene *)scene;
  mMaterials.push_back(std::make_unique<Material>());
  Material *material = mMaterials.back().get();

  // Create PBR
  {
    ccl::ShaderGraph *graph = new ccl::ShaderGraph();
    // ccl::float3 f3AlbedoColor = ccl::make_float3(albedoColor[0] * volumeAttenuationColor[0],
    //                                             albedoColor[1] * volumeAttenuationColor[1],
    //                                             albedoColor[2] * volumeAttenuationColor[2]);

    // Albedo
    ccl::float3 f3AlbedoColor = ccl::make_float3(albedoColor[0], albedoColor[1], albedoColor[2]);
    float alpha = albedoColor[3];
    ccl::ColorNode *albedoColorNode = graph->create_node<ccl::ColorNode>();
    albedoColorNode->set_value(f3AlbedoColor);
    graph->add(albedoColorNode);
    ccl::ShaderOutput *albedoOutput = nullptr;
    if (albedoTex != nullptr) {
      ccl::ImageHandle *sharedImageHandle = (ccl::ImageHandle *)albedoTex;
      material->usedImages.insert(sharedImageHandle);
      ccl::ImageTextureNode *albedoImageNode = graph->create_node<ccl::ImageTextureNode>();
      albedoImageNode->handle = *sharedImageHandle;
      SetTextureTransform(albedoImageNode, albedoTransform);
      graph->add(albedoImageNode);

      ccl::VectorMathNode *multiplyNode = graph->create_node<ccl::VectorMathNode>();
      multiplyNode->set_math_type(ccl::NODE_VECTOR_MATH_MULTIPLY);
      graph->add(multiplyNode);

      graph->connect(albedoColorNode->output("Color"), multiplyNode->input("Vector1"));
      graph->connect(albedoImageNode->output("Color"), multiplyNode->input("Vector2"));
      albedoOutput = multiplyNode->output("Vector");
    }
    else {
      albedoOutput = albedoColorNode->output("Color");
    }

    //// Alpha channel
    // ccl::SeparateColorNode *separateColorNode = graph->create_node<ccl::SeparateColorNode>();
    // graph->add(separateColorNode);
    // graph->connect(albedoOutput, separateColorNode->input("Color"));
    // ccl::ShaderOutput *alphaOutput = separateColorNode->output("Alpha");

    // Normals
    ccl::ShaderOutput *normalOutput = nullptr;
    if (normalTex != nullptr) {
      ccl::ImageHandle *sharedImageHandle = (ccl::ImageHandle *)normalTex;
      material->usedImages.insert(sharedImageHandle);
      ccl::ImageTextureNode *normalImageNode = graph->create_node<ccl::ImageTextureNode>();
      normalImageNode->handle = *sharedImageHandle;
      SetTextureTransform(normalImageNode, normalTransform);
      graph->add(normalImageNode);

      ccl::NormalMapNode *normalMapNode = graph->create_node<ccl::NormalMapNode>();
      normalMapNode->set_space(ccl::NODE_NORMAL_MAP_TANGENT);
      normalMapNode->set_strength(normalStrength);
      graph->add(normalMapNode);

      graph->connect(normalImageNode->output("Color"), normalMapNode->input("Color"));
      normalOutput = normalMapNode->output("Normal");
    }

    // Metallic and roughness
    ccl::ShaderOutput *metallicOutput = nullptr;
    ccl::ShaderOutput *roughnessOutput = nullptr;
    ccl::ValueNode *metalnessValueNode = graph->create_node<ccl::ValueNode>();
    metalnessValueNode->set_value(metallicFactor);
    graph->add(metalnessValueNode);
    ccl::ValueNode *roughnessValueNode = graph->create_node<ccl::ValueNode>();
    roughnessValueNode->set_value(roughnessFactor);
    graph->add(roughnessValueNode);

    if (metallicRoughnessTexture != nullptr) {
      ccl::ImageHandle *sharedImageHandle = (ccl::ImageHandle *)metallicRoughnessTexture;
      material->usedImages.insert(sharedImageHandle);
      ccl::ImageTextureNode *metallicRoughnessImageNode =
          graph->create_node<ccl::ImageTextureNode>();
      metallicRoughnessImageNode->handle = *sharedImageHandle;
      SetTextureTransform(metallicRoughnessImageNode, metallicRoughnessTransform);
      graph->add(metallicRoughnessImageNode);
      ccl::SeparateColorNode *separateColorNode = graph->create_node<ccl::SeparateColorNode>();
      graph->add(separateColorNode);
      graph->connect(metallicRoughnessImageNode->output("Color"),
                     separateColorNode->input("Color"));

      // Metallic
      ccl::MathNode *metalnessMultiplyNode = graph->create_node<ccl::MathNode>();
      metalnessMultiplyNode->set_math_type(ccl::NODE_MATH_MULTIPLY);
      graph->add(metalnessMultiplyNode);
      graph->connect(separateColorNode->output("Blue"), metalnessMultiplyNode->input("Value1"));
      graph->connect(metalnessValueNode->output("Value"), metalnessMultiplyNode->input("Value2"));
      metallicOutput = metalnessMultiplyNode->output("Value");

      // Roughness
      ccl::MathNode *roughnessMultiplyNode = graph->create_node<ccl::MathNode>();
      roughnessMultiplyNode->set_math_type(ccl::NODE_MATH_MULTIPLY);
      graph->add(roughnessMultiplyNode);
      graph->connect(separateColorNode->output("Green"), roughnessMultiplyNode->input("Value1"));
      graph->connect(roughnessValueNode->output("Value"), roughnessMultiplyNode->input("Value2"));
      roughnessOutput = roughnessMultiplyNode->output("Value");
    }
    else {
      metallicOutput = metalnessValueNode->output("Value");
      roughnessOutput = roughnessValueNode->output("Value");
    }

    // Emissive
    ccl::float3 f3EmissiveFactor = ccl::make_float3(
        emissiveFactor[0], emissiveFactor[1], emissiveFactor[2]);
    ccl::ColorNode *emisiveColorNode = graph->create_node<ccl::ColorNode>();
    emisiveColorNode->set_value(f3EmissiveFactor);
    graph->add(emisiveColorNode);
    ccl::ShaderOutput *emissiveOutput = nullptr;
    if (emissiveTex != nullptr) {
      ccl::ImageHandle *sharedImageHandle = (ccl::ImageHandle *)emissiveTex;
      material->usedImages.insert(sharedImageHandle);
      ccl::ImageTextureNode *emissiveImageNode = graph->create_node<ccl::ImageTextureNode>();
      emissiveImageNode->handle = *sharedImageHandle;
      SetTextureTransform(emissiveImageNode, emissiveTransform);
      graph->add(emissiveImageNode);

      ccl::VectorMathNode *multiplyNode = graph->create_node<ccl::VectorMathNode>();
      multiplyNode->set_math_type(ccl::NODE_VECTOR_MATH_MULTIPLY);
      graph->add(multiplyNode);

      graph->connect(emisiveColorNode->output("Color"), multiplyNode->input("Vector1"));
      graph->connect(emissiveImageNode->output("Color"), multiplyNode->input("Vector2"));
      emissiveOutput = multiplyNode->output("Vector");
    }
    else {
      emissiveOutput = emisiveColorNode->output("Color");
    }

    // BSDF
    ccl::PrincipledBsdfNode *bsdfNode = graph->create_node<ccl::PrincipledBsdfNode>();
    bsdfNode->set_transmission(transmissionFactor);
    bsdfNode->set_subsurface(0.0f);
    bsdfNode->set_alpha(alpha);
    if (IOR < 1.00001f)
      IOR = 1.00001f;  // clamp to this value to prevent crashes (in debug mode)
    bsdfNode->set_ior(IOR);
    // Just the same default as we have in Blender.
    // There is an extension for this stuff: KHR_materials_specular
    // https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_specular/README.md
    bsdfNode->set_specular(0.5f);
    graph->add(bsdfNode);
    ccl::ShaderOutput *bsdfOutput = bsdfNode->output("BSDF");

    // Final connections
    graph->connect(albedoOutput, bsdfNode->input("Base Color"));
    graph->connect(albedoOutput, bsdfNode->input("Subsurface Color"));
    graph->connect(metallicOutput, bsdfNode->input("Metallic"));
    graph->connect(roughnessOutput, bsdfNode->input("Roughness"));
    graph->connect(roughnessOutput, bsdfNode->input("Transmission Roughness"));
    if (normalOutput) {
      graph->connect(normalOutput, bsdfNode->input("Normal"));
    }
    graph->connect(emissiveOutput, bsdfNode->input("Emission"));
    bsdfNode->set_emission_strength(emissiveStrength);

    graph->connect(bsdfOutput, graph->output()->input("Surface"));

    ccl::Shader *shader = s->create_node<ccl::Shader>();
    shader->name = OpenImageIO_v2_4::ustring(std::string(name) + "_pbr");
    shader->set_graph(graph);
    shader->tag_update(s);

    material->pbrShader = shader;
  }

  // Create depth shader
  {
    ccl::ShaderGraph *graph = new ccl::ShaderGraph();
    ccl::GeometryNode *geometryNode = graph->create_node<ccl::GeometryNode>();
    graph->add(geometryNode);

    ccl::VectorTransformNode *cameraTransformNode = graph->create_node<ccl::VectorTransformNode>();
    cameraTransformNode->set_vector(ccl::make_float3(0.0f));
    cameraTransformNode->set_transform_type(ccl::NODE_VECTOR_TRANSFORM_TYPE_POINT);
    cameraTransformNode->set_convert_from(ccl::NODE_VECTOR_TRANSFORM_CONVERT_SPACE_CAMERA);
    cameraTransformNode->set_convert_to(ccl::NODE_VECTOR_TRANSFORM_CONVERT_SPACE_WORLD);
    graph->add(cameraTransformNode);

    ccl::VectorMathNode *distanceFromCameraNode = graph->create_node<ccl::VectorMathNode>();
    distanceFromCameraNode->set_math_type(ccl::NODE_VECTOR_MATH_DISTANCE);
    graph->add(distanceFromCameraNode);

    ccl::MathNode *ceilNode = graph->create_node<ccl::MathNode>();
    ceilNode->name = "max_depth_node";
    ceilNode->set_math_type(ccl::NODE_MATH_MINIMUM);
    ceilNode->set_value1(mMaxDepth);
    graph->add(ceilNode);

    graph->connect(geometryNode->output("Position"), distanceFromCameraNode->input("Vector1"));
    graph->connect(cameraTransformNode->output("Vector"),
                   distanceFromCameraNode->input("Vector2"));
    graph->connect(distanceFromCameraNode->output("Value"), ceilNode->input("Value2"));
    graph->connect(ceilNode->output("Value"), graph->output()->input("Surface"));

    ccl::Shader *shader = s->create_node<ccl::Shader>();
    shader->name = OpenImageIO_v2_4::ustring(std::string(name) + "_depth");
    shader->set_graph(graph);
    shader->tag_update(s);

    material->depthShader = shader;
  }

  // Create normal shader
  {
    ccl::ShaderGraph *graph = new ccl::ShaderGraph();
    ccl::ShaderOutput *normalOutput = nullptr;
    if (normalTex != nullptr) {
      ccl::ImageHandle *sharedImageHandle = (ccl::ImageHandle *)normalTex;
      material->usedImages.insert(sharedImageHandle);
      ccl::ImageTextureNode *normalImageNode = graph->create_node<ccl::ImageTextureNode>();
      normalImageNode->handle = *sharedImageHandle;
      SetTextureTransform(normalImageNode, normalTransform);
      graph->add(normalImageNode);

      ccl::NormalMapNode *normalMapNode = graph->create_node<ccl::NormalMapNode>();
      normalMapNode->set_space(ccl::NODE_NORMAL_MAP_TANGENT);
      normalMapNode->set_strength(normalStrength);
      graph->add(normalMapNode);

      graph->connect(normalImageNode->output("Color"), normalMapNode->input("Color"));
      normalOutput = normalMapNode->output("Normal");
    }
    else
    {
      ccl::GeometryNode *geometryNode = graph->create_node<ccl::GeometryNode>();
      graph->add(geometryNode);
      normalOutput = geometryNode->output("Normal");
    }

    ccl::VectorTransformNode *objectTransformNode = graph->create_node<ccl::VectorTransformNode>();
    objectTransformNode->set_transform_type(ccl::NODE_VECTOR_TRANSFORM_TYPE_NORMAL);
    objectTransformNode->set_convert_from(ccl::NODE_VECTOR_TRANSFORM_CONVERT_SPACE_WORLD);
    objectTransformNode->set_convert_to(ccl::NODE_VECTOR_TRANSFORM_CONVERT_SPACE_CAMERA);
    graph->add(objectTransformNode);

    graph->connect(normalOutput, objectTransformNode->input("Vector"));
    graph->connect(objectTransformNode->output("Vector"), graph->output()->input("Surface"));

    ccl::Shader *shader = s->create_node<ccl::Shader>();
    shader->name = OpenImageIO_v2_4::ustring(std::string(name) + "_normal");
    shader->set_graph(graph);
    shader->tag_update(s);

    material->normalShader = shader;
  }

  // Create albedo shader
  {
    ccl::ShaderGraph *graph = new ccl::ShaderGraph();
    // ccl::float3 f3AlbedoColor = ccl::make_float3(albedoColor[0] * volumeAttenuationColor[0],
    //                                             albedoColor[1] * volumeAttenuationColor[1],
    //                                             albedoColor[2] * volumeAttenuationColor[2]);
    ccl::float3 f3AlbedoColor = ccl::make_float3(albedoColor[0], albedoColor[1], albedoColor[2]);
    float alpha = albedoColor[3];
    ccl::ColorNode *albedoColorNode = graph->create_node<ccl::ColorNode>();

    albedoColorNode->set_value(f3AlbedoColor);
    graph->add(albedoColorNode);

    // Albedo
    ccl::ShaderOutput *albedoOutput = nullptr;
    if (albedoTex != nullptr) {
      ccl::ImageHandle *sharedImageHandle = (ccl::ImageHandle *)albedoTex;
      material->usedImages.insert(sharedImageHandle);
      ccl::ImageTextureNode *albedoImageNode = graph->create_node<ccl::ImageTextureNode>();
      albedoImageNode->handle = *sharedImageHandle;
      SetTextureTransform(albedoImageNode, albedoTransform);
      graph->add(albedoImageNode);

      ccl::VectorMathNode *multiplyNode = graph->create_node<ccl::VectorMathNode>();
      multiplyNode->set_math_type(ccl::NODE_VECTOR_MATH_MULTIPLY);
      graph->add(multiplyNode);

      graph->connect(albedoColorNode->output("Color"), multiplyNode->input("Vector1"));
      graph->connect(albedoImageNode->output("Color"), multiplyNode->input("Vector2"));
      albedoOutput = multiplyNode->output("Vector");
    }
    else {
      albedoOutput = albedoColorNode->output("Color");
    }

    graph->connect(albedoOutput, graph->output()->input("Surface"));

    ccl::Shader *shader = s->create_node<ccl::Shader>();
    shader->name = OpenImageIO_v2_4::ustring(std::string(name) + "_albedo");
    shader->set_graph(graph);
    shader->tag_update(s);

    material->albedoShader = shader;
  }

  return material;
}

Mesh *CyclesEngine::AddMesh(Scene *scene,
                            const char *name,
                            Material **materials,
                            float *vertexPosArray,
                            float *vertexNormalArray,
                            float *vertexUVArray,
                            uint vertexCount,
                            uint *indices,
                            uint *triangleCounts,
                            uint submeshCount)
{
  // Create mesh
  ccl::Scene *s = (ccl::Scene *)scene;
  ccl::Mesh *mesh = new ccl::Mesh();  // cycles should take care of deleting this object
  s->geometry.push_back(mesh);

  size_t totalTriangleCount = 0;
  for (size_t i = 0; i < submeshCount; i++) {
    totalTriangleCount += triangleCounts[i];
  }

  // Set mesh properties
  mesh->name = name;
  mesh->reserve_mesh(vertexCount, totalTriangleCount);
  mesh->set_subdivision_type(ccl::Mesh::SUBDIVISION_LINEAR);

  // Set vertices
  ccl::array<ccl::float3> P_array;
  P_array.resize(vertexCount);
  for (size_t i = 0; i < vertexCount; i++) {
    P_array[i] = ccl::make_float3(
        vertexPosArray[i * 3 + 0], vertexPosArray[i * 3 + 1], vertexPosArray[i * 3 + 2]);
  }
  mesh->set_verts(P_array);

  // Create submeshes
  size_t currentStartingTriangleIndex = 0;
  for (size_t i = 0; i < submeshCount; i++) {
    // Create triangles
    for (size_t j = 0; j < triangleCounts[i]; j++) {

      int v0 = (currentStartingTriangleIndex + j) * 3 + 0;
      int v1 = (currentStartingTriangleIndex + j) * 3 + 1;
      int v2 = (currentStartingTriangleIndex + j) * 3 + 2;
      mesh->add_triangle((int)indices[v0], (int)indices[v1], (int)indices[v2], i, true);
    }
    currentStartingTriangleIndex += triangleCounts[i];
  }

  // Set normals, first face normals
  mesh->add_face_normals();
  // Then either set or compute vertex normals
  ccl::float3 *fdataNormal = nullptr;
  if (vertexNormalArray != nullptr) {
    ccl::Attribute *nAttr = mesh->attributes.add(ccl::ATTR_STD_VERTEX_NORMAL);
    fdataNormal = nAttr->data_float3();

    for (size_t i = 0; i < vertexCount; i++) {
      fdataNormal[i] = ccl::make_float3(vertexNormalArray[i * 3 + 0],
                                        vertexNormalArray[i * 3 + 1],
                                        vertexNormalArray[i * 3 + 2]);
      //fdataNormal[i] = ccl::normalize(fdataNormal[i]);
    }
  }
  else {
    mesh->add_vertex_normals();
    ccl::Attribute *nAttr = mesh->attributes.find(ccl::ATTR_STD_VERTEX_NORMAL);
    fdataNormal = nAttr->data_float3();
  }

  // Set UVs
  ccl::float2 *fdataUV = nullptr;
  if (vertexUVArray != nullptr) {
    ccl::Attribute *uvAttr = mesh->attributes.add(ccl::ATTR_STD_UV);
    fdataUV = uvAttr->data_float2();

    // Loop over triangles
    size_t currentStartingTriangleIndex = 0;
    for (size_t i = 0; i < submeshCount; i++) {
      for (size_t j = 0; j < triangleCounts[i]; j++) {
        int j0 = (currentStartingTriangleIndex + j) * 3 + 0;
        int j1 = (currentStartingTriangleIndex + j) * 3 + 1;
        int j2 = (currentStartingTriangleIndex + j) * 3 + 2;
        int i0 = indices[j0];
        int i1 = indices[j1];
        int i2 = indices[j2];

        fdataUV[j0] = ccl::make_float2(vertexUVArray[i0 * 2 + 0],
                                       1.0f - vertexUVArray[i0 * 2 + 1]);
        fdataUV[j1] = ccl::make_float2(vertexUVArray[i1 * 2 + 0],
                                       1.0f - vertexUVArray[i1 * 2 + 1]);
        fdataUV[j2] = ccl::make_float2(vertexUVArray[i2 * 2 + 0],
                                       1.0f - vertexUVArray[i2 * 2 + 1]);
      }
      currentStartingTriangleIndex += triangleCounts[i];
    }
  }

  // Set Tangents
  bool setTangents = (fdataNormal != nullptr) && (fdataUV != nullptr);
  if (setTangents) {
    ccl::Attribute *attrTangent = mesh->attributes.add(ccl::ATTR_STD_UV_TANGENT);
    ccl::Attribute *attrTangentSign = mesh->attributes.add(ccl::ATTR_STD_UV_TANGENT_SIGN);
    ccl::float3 *fdataTangent = attrTangent->data_float3();
    float *fdataTangentSign = attrTangentSign->data_float();
    ccl::array<ccl::float3> &vertices = mesh->get_verts();

    size_t currentStartingTriangleIndex = 0;
    for (size_t i = 0; i < submeshCount; i++) {
      for (long j = 0; j < triangleCounts[i]; j++) {
        int j0 = (currentStartingTriangleIndex + j) * 3 + 0;
        int j1 = (currentStartingTriangleIndex + j) * 3 + 1;
        int j2 = (currentStartingTriangleIndex + j) * 3 + 2;

        int i0 = indices[j0];
        int i1 = indices[j1];
        int i2 = indices[j2];

        const ccl::float3 &v1 = vertices[i0];
        const ccl::float3 &v2 = vertices[i1];
        const ccl::float3 &v3 = vertices[i2];

        const ccl::float2 &w1 = fdataUV[j0];
        const ccl::float2 &w2 = fdataUV[j1];
        const ccl::float2 &w3 = fdataUV[j2];

        const ccl::float3 &n1 = fdataNormal[i0];
        const ccl::float3 &n2 = fdataNormal[i1];
        const ccl::float3 &n3 = fdataNormal[i2];

        float x1 = v2.x - v1.x;
        float x2 = v3.x - v1.x;
        float y1 = v2.y - v1.y;
        float y2 = v3.y - v1.y;
        float z1 = v2.z - v1.z;
        float z2 = v3.z - v1.z;

        float s1 = w2.x - w1.x;
        float s2 = w3.x - w1.x;
        float t1 = w2.y - w1.y;
        float t2 = w3.y - w1.y;

        float r = 1.0F / (s1 * t2 - s2 * t1);
        ccl::float3 sdir = ccl::make_float3((t2 * x1 - t1 * x2) * r, (t2 * y1 - t1 * y2) * r, (t2 * z1 - t1 * z2) * r);
        ccl::float3 tdir = ccl::make_float3((s1 * x2 - s2 * x1) * r, (s1 * y2 - s2 * y1) * r, (s1 * z2 - s2 * z1) * r);
        ccl::float3 tan = sdir;
        ccl::float3 bitan = tdir;
        
        // Gram-Schmidt orthogonalize
        fdataTangent[j0] = ccl::normalize(tan - n1 * ccl::dot(n1, tan));
        fdataTangent[j1] = ccl::normalize(tan - n2 * ccl::dot(n2, tan));
        fdataTangent[j2] = ccl::normalize(tan - n3 * ccl::dot(n3, tan));

        // Calculate handedness
        fdataTangentSign[j0] = (ccl::dot(ccl::cross(n1, tan), bitan) < 0.0F) ? -1.0F : 1.0F;
        fdataTangentSign[j1] = (ccl::dot(ccl::cross(n2, tan), bitan) < 0.0F) ? -1.0F : 1.0F;
        fdataTangentSign[j2] = (ccl::dot(ccl::cross(n3, tan), bitan) < 0.0F) ? -1.0F : 1.0F;
      }
      currentStartingTriangleIndex += triangleCounts[i];
    }
  }

  // Set shaders
  ccl::array<ccl::Node *> used_shaders;  // = mesh->get_used_shaders();
  for (size_t i = 0; i < submeshCount; i++) {
    ccl::Shader *shader = materials[i] ? (ccl::Shader *)materials[i]->pbrShader :
                                         mNameToShader[sDefaultSurfaceShaderName];
    used_shaders.push_back_slow(shader);
  }
  mesh->set_used_shaders(used_shaders);
  mesh->tag_update(s, false);

  return (cycles_wrapper::Mesh *)mesh;
}

void CyclesEngine::UpdateMeshMaterials(
    Scene *scene, Mesh *mesh, Material **materials, uint submeshCount, RenderMode renderMode)
{
  ccl::Scene *s = (ccl::Scene *)scene;
  ccl::Mesh *m = (ccl::Mesh *)mesh;

  //// Set some flag for the old shaders
  //ccl::array<ccl::Node *> used_shaders_old = m->get_used_shaders();
  //for (size_t i = 0; i < used_shaders_old.size(); i++) {
  //  ccl::Shader *shader = (ccl::Shader *)used_shaders_old[i];
  //  shader->tag_used(s);
  //  shader->tag_update(s);
  //  shader->tag_modified();
  //}

  // Collect shaders
  ccl::array<ccl::Node *> used_shaders;
  for (size_t i = 0; i < submeshCount; i++) {
    ccl::Shader *shader;
    switch (renderMode) {
      case cycles_wrapper::Depth: {
        // First get the shader
        shader = (ccl::Shader *)materials[i]->depthShader;
        // Then update the max depth value
        ccl::MathNode *node = nullptr;
        for (auto &it : shader->graph->nodes) {
          auto castedNode = dynamic_cast<ccl::MathNode *>(it);
          if (castedNode && castedNode->name.compare("max_depth_node") == 0)
          {
            node = castedNode;
            break;
          }
        }

        if (node) {
          bool change = node->get_value1() != mMaxDepth;
          if (change) {
            node->set_value1(mMaxDepth);
            ccl::Scene *scene = mOptions.session->scene;
            shader->tag_update(scene);
          }
        }
      }
        break;
      case cycles_wrapper::Normal:
        shader = (ccl::Shader *)materials[i]->normalShader;
        break;
      case cycles_wrapper::Albedo:
        shader = (ccl::Shader *)materials[i]->albedoShader;
        break;
      case cycles_wrapper::PBR:
      default:
        shader = (ccl::Shader *)materials[i]->pbrShader;
        break;
    }
    shader->tag_used(s);
    shader->tag_modified();
    used_shaders.push_back_slow(shader);
  }

  // Set shaders and set appropriate tags
  m->set_used_shaders(used_shaders);
  m->tag_used_shaders_modified();
  m->tag_modified();
  m->tag_update(s, true);
}

Light *CyclesEngine::AddLightToNode(Scene *scene,
                                    Node *node,
                                    int type,
                                    float *color,
                                    float intensity,
                                    float range,
                                    float innerConeAngle,
                                    float outerConeAngle)
{
  if (!scene || !node)
    return nullptr;

  ccl::Scene *s = (ccl::Scene *)scene;
  ccl::Light *light = new ccl::Light();
  s->lights.push_back(light);
  light->set_cast_shadow(true);
  light->set_use_transmission(true);
  light->set_use_caustics(true);
  light->set_normalize(true);

  // Transformation data
  ccl::Transform *nodeTransform = node->transform.get();
  ccl::float3 dir = ccl::make_float3(0.0f, 0.0f, -1.0f);
  dir = ccl::transform_direction(nodeTransform, dir);
  ccl::float3 pos = ccl::make_float3(0.0f);
  pos = ccl::transform_point(nodeTransform, pos);
  ccl::float3 axisu = ccl::cross(dir, ccl::make_float3(0.0f, -1.0f, 0.0f));
  axisu = ccl::normalize(axisu);
  ccl::float3 axisv = ccl::cross(dir, axisu);
  axisv = ccl::normalize(axisv);
  light->set_dir(dir);
  light->set_co(pos);
  light->set_axisu(axisu);
  light->set_axisv(axisv);
  light->set_tfm(*nodeTransform);
  light->set_owner(s);

  // Luminous efficacy (lumenToWatt) of an ideal monochromatic source: 555 nm:
  // https://en.wikipedia.org/wiki/Luminous_efficacy
  const float lumenToWatt = 1.0f / 683.002f;
  const float candelaToWattFactor = 4.0f * M_PI * lumenToWatt;
  const float luxToWattFactor = 1.0f * lumenToWatt;
  ccl::float3 strength = ccl::make_float3(color[0], color[1], color[2]);
  switch (type) {
    case 0:  // directional
      light->set_light_type(ccl::LIGHT_DISTANT);
      // for directional light the intensity is measured in lux (lm/m2)
      strength *= intensity * luxToWattFactor;
      light->set_angle(0.009180f);  // hard coded to match the angle from Blender
      break;

    case 1:  // spot
      light->set_light_type(ccl::LIGHT_SPOT);
      // for spot light the intensity is measured in candela (lm/sr)
      strength *= intensity * candelaToWattFactor;
      light->set_size(0.01f);
      light->set_spot_angle(outerConeAngle *
                            2);  // glTF defines the angle as half as the cycles angle
      light->set_spot_smooth((outerConeAngle - innerConeAngle) / outerConeAngle);
      break;

    case 2:  // point
      light->set_light_type(ccl::LIGHT_POINT);
      // for point light the intensity is measured in candela (lm/sr)
      strength *= intensity * candelaToWattFactor;
      light->set_size(0.01f);
      break;

    default:
      throw;
      break;
  }

  light->set_shader(mNameToShader[sLightShaderName]);
  light->set_strength(strength);
  light->tag_update(s);
  node->assignedLightObjects.push_back(light);
  return (cycles_wrapper::Light *)light;
}

bool CyclesEngine::RemoveLightFromNode(Scene *scene, Node *node, Light *light)
{
  if (!scene || !node || !light)
    return false;

  ccl::Scene *s = (ccl::Scene *)scene;
  ccl::Light *l = (ccl::Light *)light;
  int eraseCount = 0;
  size_t sizeBefore = s->lights.size();
  s->delete_node(l);
  size_t sizeAfter = s->lights.size();
  if (sizeBefore == (sizeAfter + 1)) eraseCount++;
  for (size_t i = 0; i < node->assignedLightObjects.size(); i++) {
    if (node->assignedLightObjects[i] == l) {
      node->assignedLightObjects.erase(node->assignedLightObjects.begin() + i);
      eraseCount++;
      break;
    }
  }
  return (eraseCount == 2);
}

bool CyclesEngine::AssignMeshToNode(Scene *scene, Node *node, Mesh *mesh)
{
  if (!scene || !node || !mesh) {
      return false;
  }
  ccl::Scene *s = (ccl::Scene *)scene;
  ccl::Transform *nodeTransform = node->transform.get();
  ccl::Mesh *cyclesMesh = (ccl::Mesh *)mesh;
  cyclesMesh->tag_update(s, true);
  ccl::Object *object = new ccl::Object();  // cycles should take care of deleting this object
  object->name = node->name;
  object->set_geometry(cyclesMesh);
  // object->set_is_caustics_caster(node->name.compare("Sphere") == 0);
  object->set_is_caustics_receiver(true);
  object->set_tfm(*nodeTransform);
  object->set_owner(s);
  object->tag_update(s);
  s->objects.push_back(object);
  node->assignedMeshObject = object;
  return true;
}
