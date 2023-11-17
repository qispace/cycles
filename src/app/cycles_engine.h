
#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <functional>

#ifdef CYCLES_LIB_EXPORTS
#  define DLL_API __declspec(dllexport)
#else
#  define DLL_API __declspec(dllimport)
#endif

#define LOG_TYPE_DEBUG 0
#define LOG_TYPE_INFO 1
#define LOG_TYPE_WARNING 2
#define LOG_TYPE_ERROR 3

namespace ccl {

class Session;
class Scene;
class Shader;
class ShaderGraph;
class SceneParams;
class ImageHandle;
class SessionParams;
class BufferParams;
struct Transform;
class Mesh;
class Object;
class Light;

}  // namespace ccl

namespace cycles_wrapper {

typedef unsigned long long QiObjectID;
typedef unsigned int uint;

struct Texture {};
struct TextureTransform {
  float offset[2];
  float rotation;
  float scale[2];
};
struct Mesh {};
struct Light {};
struct Scene {};
struct Node {
  Scene *scene = nullptr;
  std::string name;
  Node *parent = nullptr;
  std::vector<Node *> children;
  std::unique_ptr<ccl::Transform> transform;
  float t[3];
  float r[4];
  float s[3];
  bool visible = true;
  ccl::Object *assignedMeshObject = nullptr;
  std::vector<ccl::Light *> assignedLightObjects;
};
enum RenderMode { PBR, Depth, Normal, Albedo };
struct Material {
  ccl::Shader *pbrShader, *depthShader, *normalShader, *albedoShader;
  std::set<ccl::ImageHandle *> usedImages;
};
enum CameraType { Perspective, Orthographic, Panoramic };
struct BackgroundSettings {
  enum class Type { Color, Sky } mType;
  union {
    float mColor[3];
    struct Sky {
      float mSunDirection[3];
    } mSky;
  };
};
struct Options {
  std::unique_ptr<ccl::Session> session;
  std::unique_ptr<ccl::SceneParams> scene_params;
  std::unique_ptr<ccl::SessionParams> session_params;
  int width, height;
  bool quiet;
  bool show_help, interactive, pause;
  std::string output_pass;
};

class CyclesEngine {

  typedef void (*LogFunctionPtr)(void *, int, const char *);

 public:
  DLL_API CyclesEngine();
  DLL_API virtual ~CyclesEngine();

  DLL_API virtual bool SessionInit();
  DLL_API virtual bool SessionExit();
  DLL_API virtual void PostSceneUpdate();
  DLL_API void SetLogFunction(void *, LogFunctionPtr);
  DLL_API int GetViewportWidth();
  DLL_API int GetViewportHeight();
  DLL_API void Resize(unsigned int width, unsigned int height);
  DLL_API void SetCamera(float p[], float d[], float u[], CameraType cameraType);
  DLL_API void GetCamera(
      float p[], float d[], float u[], float *n, float *f, float *fov, float *aspect);

  // Scene graph manipulation
  DLL_API Scene *GetScene();
  DLL_API void CleanScene(Scene *scene);
  DLL_API void ClearScene(Scene *scene);
  DLL_API void SetSceneMaxDepth(float maxDepth);
  DLL_API void SetSceneBackground(const BackgroundSettings &);
  DLL_API Node *AddNode(Scene *scene,
                        const std::string &name,
                        Node *parent,
                        QiObjectID qiId,
                        float t[3],
                        float r[4],
                        float s[3]);
  DLL_API Node *GetNode(QiObjectID qiId);
  DLL_API void RemoveNode(Node *node);
  DLL_API void UpdateNodeTransform(Node *node, float t[3], float r[4], float s[3]);
  DLL_API void UpdateNodeVisibility(Node *node, bool visible);
  DLL_API Texture *AddTexture(Scene *scene,
                              const char *name,
                              const unsigned char *data,
                              size_t dataSize,
                              const char *mimeType,
                              bool isSRGB);
  DLL_API Material *AddMaterial(Scene *scene,
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
                                float volumeAttenuationDistance);
  DLL_API Mesh *AddMesh(Scene *scene,
                        const char *name,
                        Material **materials,
                        float *vertexPosArray,
                        float *vertexNormalArray,
                        float *vertexUVArray,
                        uint vertexCount,
                        uint *indices,
                        uint *triangleCounts,
                        uint submeshCount);
  DLL_API void UpdateMeshMaterials(Scene *scene,
                                   Mesh *mesh,
                                   Material **materials,
                                   uint submeshCount,
                                   RenderMode renderMode = RenderMode::PBR);
  DLL_API Light *AddLightToNode(Scene *scene,
                                Node *node,
                                int type,
                                float *color,
                                float intensity,
                                float range,
                                float innerConeAngle,
                                float outerConeAngle);
  DLL_API bool RemoveLightFromNode(Scene *scene, Node *node, Light *light);
  DLL_API bool AssignMeshToNode(Scene *scene, Node *node, Mesh *mesh);

 protected:
  void Log(int type, const std::string &msg);
  ccl::BufferParams GetBufferParams();
  virtual void DefaultSceneInit();
  virtual void ResetSession();
  virtual void CancelSession();

 protected:
  Options mOptions;
  int mViewportWidth;
  int mViewportHeight;
  int mCurrentSample;
  float mMaxDepth;
  std::vector<std::unique_ptr<ccl::ImageHandle>> mImageHandles;

  // Camera cache
  std::unique_ptr<ccl::Transform> mCameraTransform;
  CameraType mCameraType;

  // Scene structures
  std::vector<std::unique_ptr<Node>> mNodes;
  std::vector<std::unique_ptr<Material>> mMaterials;
  std::map<QiObjectID, Node *> mQiIDToNode;
  std::map<std::string, ccl::Shader *> mNameToShader;

  // Static const
  static const std::string sDefaultSurfaceShaderName;
  static const std::string sLightShaderName;
  static const std::string sDisabledLightShaderName;
  static const std::string sTexturedShaderName;
  static const std::string sColorBackgroundShaderName;
  static const std::string sSkyBackgroundShaderName;

 protected:
  std::string mCurrentBackgroundShaderName;

 private:
  void *mLogObjectPtr = nullptr;
  LogFunctionPtr mLogFunctionPtr = nullptr;
};

}  // namespace cycles_wrapper
