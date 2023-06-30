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
#include "session/buffers.h"
#include "session/session.h"
#include "app/cycles_xml.h"


using namespace cycles_wrapper;

const std::string CyclesEngine::sLightShaderName = "qi_shader_light";
const std::string CyclesEngine::sBackgroundShaderName = "qi_shader_background";

void CyclesEngine::DefaultSceneInit()
{
  // Set up shaders
  ccl::Scene *scene = mOptions.session->scene;
  mNameToShader.clear();
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
  // Background (set as default background)
  {
    ccl::ShaderGraph *graph = new ccl::ShaderGraph();

    ccl::BackgroundNode *backgroundNode = graph->create_node<ccl::BackgroundNode>();
    //backgroundNode->set_color(ccl::make_float3(FLT_MAX, FLT_MAX, FLT_MAX));
    backgroundNode->set_color(ccl::make_float3(0.0f, 0.0f, 0.0f));
    backgroundNode->set_strength(1.0f);
    graph->add(backgroundNode);

    graph->connect(backgroundNode->output("Background"), graph->output()->input("Surface"));

    ccl::Shader *shader = scene->create_node<ccl::Shader>();
    shader->name = sBackgroundShaderName;
    shader->set_graph(graph);
    shader->reference();
    shader->tag_update(scene);
    graph->simplified = true; // prevent further simplification and node removal
    mNameToShader[shader->name.c_str()] = shader;
    scene->default_background = shader;
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
    if (shadersToKeep.find((*matVectorIte)->colorShader) == shadersToKeep.end() &&
        shadersToKeep.find((*matVectorIte)->depthShader) == shadersToKeep.end()) {
      s->delete_node(matVectorIte->get()->colorShader);
      s->delete_node(matVectorIte->get()->depthShader);
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

 void CyclesEngine::SetSceneBackgroundColor(float *color)
{
  ccl::Shader *shader = mNameToShader[sBackgroundShaderName];
  ccl::BackgroundNode *backgroundNode = nullptr;
  for (auto &it : shader->graph->nodes) {
    backgroundNode = dynamic_cast<ccl::BackgroundNode *>(it);
    if (backgroundNode)
      break;
  }

  if (backgroundNode) {
    backgroundNode->set_color(ccl::make_float3(color[0], color[1], color[2]));
    ccl::Scene *scene = mOptions.session->scene;
    shader->tag_update(scene);
  }
  else {
    Log(LOG_TYPE_WARNING,
        "Failed updating the background shader. BackgroundNode not found.");
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

  // Now remove this node
  ccl::Scene *s = (ccl::Scene *)node->scene;
  if (node->assignedMeshObject)
    s->delete_node(node->assignedMeshObject);
  if (node->assignedLightObject)
    s->delete_node(node->assignedLightObject);

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

void CyclesEngine::UpdateNode(Node *node, float t[3], float r[4], float s[3])
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

    if (node->assignedLightObject) {
    ccl::float3 dir = ccl::make_float3(0.0f, 0.0f, -1.0f);
    dir = ccl::transform_direction(transform, dir);
    ccl::float3 pos = ccl::make_float3(0.0f);
    pos = ccl::transform_point(transform, pos);
    ccl::float3 axisu = ccl::cross(dir, ccl::make_float3(0.0f, -1.0f, 0.0f));
    axisu = ccl::normalize(axisu);
    ccl::float3 axisv = ccl::cross(dir, axisu);
    axisv = ccl::normalize(axisv);
    ccl::Light *l = (ccl::Light *)node->assignedLightObject;
    l->set_dir(dir);
    l->set_co(pos);
    l->set_axisu(axisu);
    l->set_axisv(axisv);
    l->set_tfm(*transform);
    l->tag_update(scene);
  }

  memcpy_s(node->t, sizeof(float) * 3, t, sizeof(float) * 3);
  memcpy_s(node->s, sizeof(float) * 3, s, sizeof(float) * 3);
  memcpy_s(node->r, sizeof(float) * 4, r, sizeof(float) * 4);

  // Update the children now as well
  for (size_t i = 0; i < node->children.size(); i++) {
    Node *childNode = node->children[i];
    UpdateNode(childNode, childNode->t, childNode->r, childNode->s);
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

Material *CyclesEngine::AddMaterial(Scene *scene,
                                    const char *name,
                                    Texture *albedoTex,
                                    float *albedoColor,
                                    Texture *metallicRoughnessTexture,
                                    float metallicFactor,
                                    float roughnessFactor,
                                    Texture *normalTex,
                                    float normalStrength,
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

  // Create color shader
  {
    ccl::ShaderGraph *graph = new ccl::ShaderGraph();

    // Compute the albedo color
    // ccl::float3 f3AlbedoColor = ccl::make_float3(albedoColor[0] * volumeAttenuationColor[0],
    //                                             albedoColor[1] * volumeAttenuationColor[1],
    //                                             albedoColor[2] * volumeAttenuationColor[2]);
    ccl::float3 f3AlbedoColor = ccl::make_float3(albedoColor[0], albedoColor[1], albedoColor[2]);
    float alpha = albedoColor[3];
    ccl::ColorNode *albedoColorNode = graph->create_node<ccl::ColorNode>();

    albedoColorNode->set_value(f3AlbedoColor);
    graph->add(albedoColorNode);

    // Ablbedo
    ccl::ShaderOutput *albedoOutput = nullptr;
    if (albedoTex != nullptr) {
      ccl::ImageHandle *sharedImageHandle = (ccl::ImageHandle *)albedoTex;
      ccl::ImageTextureNode *albedoImageNode = graph->create_node<ccl::ImageTextureNode>();
      albedoImageNode->handle = *sharedImageHandle;
      material->usedImages.insert(sharedImageHandle);
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
      ccl::ImageTextureNode *normalImageNode = graph->create_node<ccl::ImageTextureNode>();
      normalImageNode->handle = *sharedImageHandle;
      material->usedImages.insert(sharedImageHandle);
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
      ccl::ImageTextureNode *metallicRoughnessImageNode =
          graph->create_node<ccl::ImageTextureNode>();
      metallicRoughnessImageNode->handle = *sharedImageHandle;
      material->usedImages.insert(sharedImageHandle);
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

    // Depth
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

    graph->connect(geometryNode->output("Position"), distanceFromCameraNode->input("Vector1"));
    graph->connect(cameraTransformNode->output("Vector"),
                   distanceFromCameraNode->input("Vector2"));
    ccl::ShaderOutput *depthOutput = distanceFromCameraNode->output("Value");

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
    graph->connect(bsdfOutput, graph->output()->input("Surface"));

    ccl::Shader *shader = s->create_node<ccl::Shader>();
    shader->name = OpenImageIO_v2_4::ustring(name);
    shader->set_graph(graph);
    shader->tag_update(s);

    material->colorShader = shader;
    //return (cycles_wrapper::Material *)shader;
  }

  // Create depth shader
  {
    ccl::ShaderGraph *graph = new ccl::ShaderGraph();

    // Depth
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
  size_t currentStartingIndex = 0;
  for (size_t i = 0; i < submeshCount; i++) {
    // Create triangles
    for (size_t j = 0; j < triangleCounts[i]; j++) {

      int v0 = currentStartingIndex + j * 3 + 0;
      int v1 = currentStartingIndex + j * 3 + 1;
      int v2 = currentStartingIndex + j * 3 + 2;
      mesh->add_triangle((int)indices[v0], (int)indices[v1], (int)indices[v2], i, true);
    }
    currentStartingIndex += triangleCounts[i] * 3;
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
    }
  }
  else {
    mesh->add_vertex_normals();
  }

  // Set UVs
  ccl::float2 *fdataUV = nullptr;
  if (vertexUVArray != nullptr) {
    ccl::Attribute *uvAttr = mesh->attributes.add(ccl::ATTR_STD_UV);
    fdataUV = uvAttr->data_float2();

    // Loop over triangles
    size_t currentStartingIndex = 0;
    for (size_t i = 0; i < submeshCount; i++) {
      for (size_t j = 0; j < triangleCounts[i]; j++) {
        int j0 = currentStartingIndex + j * 3 + 0;
        int j1 = currentStartingIndex + j * 3 + 1;
        int j2 = currentStartingIndex + j * 3 + 2;
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
      currentStartingIndex += triangleCounts[i] * 3;
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

    size_t currentStartingIndex = 0;
    for (size_t i = 0; i < submeshCount; i++) {
      for (long j = 0; j < triangleCounts[i]; j++) {
        int j0 = currentStartingIndex + j * 3 + 0;
        int j1 = currentStartingIndex + j * 3 + 1;
        int j2 = currentStartingIndex + j * 3 + 2;

        int i1 = indices[j0];
        int i2 = indices[j1];
        int i3 = indices[j2];

        const ccl::float3 &v1 = vertices[i1];
        const ccl::float3 &v2 = vertices[i2];
        const ccl::float3 &v3 = vertices[i3];

        const ccl::float2 &w1 = fdataUV[j0];
        const ccl::float2 &w2 = fdataUV[j1];
        const ccl::float2 &w3 = fdataUV[j2];

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
        ccl::float3 sdir = ccl::make_float3(
            (t2 * x1 - t1 * x2) * r, (t2 * y1 - t1 * y2) * r, (t2 * z1 - t1 * z2) * r);
        ccl::float3 tdir = ccl::make_float3(
            (s1 * x2 - s2 * x1) * r, (s1 * y2 - s2 * y1) * r, (s1 * z2 - s2 * z1) * r);

        ccl::float3 tan = sdir;
        ccl::float3 bitan = tdir;

        const ccl::float3 &n1 = fdataNormal[i1];
        const ccl::float3 &n2 = fdataNormal[i2];
        const ccl::float3 &n3 = fdataNormal[i3];

        // Gram-Schmidt orthogonalize
        fdataTangent[j0] = ccl::normalize(tan - n1 * ccl::dot(n1, tan));
        fdataTangent[j1] = ccl::normalize(tan - n2 * ccl::dot(n2, tan));
        fdataTangent[j2] = ccl::normalize(tan - n3 * ccl::dot(n3, tan));

        // Calculate handedness
        fdataTangentSign[j0] = (ccl::dot(ccl::cross(n1, tan), bitan) < 0.0F) ? -1.0F : 1.0F;
        fdataTangentSign[j1] = (ccl::dot(ccl::cross(n2, tan), bitan) < 0.0F) ? -1.0F : 1.0F;
        fdataTangentSign[j2] = (ccl::dot(ccl::cross(n3, tan), bitan) < 0.0F) ? -1.0F : 1.0F;
      }
      currentStartingIndex += triangleCounts[i] * 3;
    }
  }

  // Set shaders
  ccl::array<ccl::Node *> used_shaders; // = mesh->get_used_shaders();
  for (size_t i = 0; i < submeshCount; i++) {
    ccl::Shader *shader = (ccl::Shader *)materials[i]->colorShader;
    used_shaders.push_back_slow(shader);
  }
  mesh->set_used_shaders(used_shaders);
  //mesh->tag_update(s, false);

  return (cycles_wrapper::Mesh *)mesh;
}

void CyclesEngine::SetMeshRenderMode(
    Scene *scene, Mesh *mesh, Material **materials, uint submeshCount, RenderMode renderMode)
{
  ccl::Scene *s = (ccl::Scene *)scene;
  ccl::Mesh *m = (ccl::Mesh *)mesh;

  // Set shaders
  ccl::array<ccl::Node *> used_shaders;  // = mesh->get_used_shaders();
  for (size_t i = 0; i < submeshCount; i++) {
    ccl::Shader *shader;
    switch (renderMode) {
      case cycles_wrapper::Depth:
        shader = (ccl::Shader *)materials[i]->depthShader;
        break;
      case cycles_wrapper::Normal:
      case cycles_wrapper::Color:
      default:
        shader = (ccl::Shader *)materials[i]->colorShader;
        break;
    }
    used_shaders.push_back_slow(shader);
  }

    m->set_used_shaders(used_shaders);
    m->tag_update(s, false);
}

Light *CyclesEngine::AssignLightToNode(Scene *scene,
                                       Node *node,
                                       const char *name,
                                       int type,
                                       float *color,
                                       float intensity,
                                       float range,
                                       float innerConeAngle,
                                       float outerConeAngle)
{
  if (!scene || !node)
    return false;

  if (node->assignedLightObject)
    return false; // can't have an already assigned object

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
  node->assignedLightObject = light;
  return (cycles_wrapper::Light *)light;
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
