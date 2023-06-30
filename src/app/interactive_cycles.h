
#pragma once
#include "cycles_engine.h"

#define ACQUIRE_LOCK _Acquires_exclusive_lock_(*this->mOpenGLContextLockPtr)
#define RELEASE_LOCK _Releases_exclusive_lock_(*this->mOpenGLContextLockPtr)

namespace cycles_wrapper {

struct DisplayDriverDelegateParams {

  void *gl_upload_sync_ = nullptr;
  void *gl_render_sync_ = nullptr;
  bool useNearestPointSampling;
  unsigned int texId;
  unsigned int vertexBufferId;
  int texcoordAttribute;
  int positionAttribute;
};

class InteractiveCycles : public CyclesEngine {

  friend class InteractiveCyclesOpenGLDisplayDriver;
  friend class InteractiveCyclesOutputDriver;

 public:
  DLL_API InteractiveCycles();
  DLL_API virtual ~InteractiveCycles();

  DLL_API bool InitializeOpenGL(void *windowHWND, void *hDC, void *hRC, void *contextLock);
  DLL_API bool InitializeOpenGL(void *windowHWND);
  DLL_API bool DeinitializeOpenGL();
  DLL_API bool IsOpenGLInitialized();
  DLL_API virtual bool SessionInit();
  DLL_API virtual bool SessionExit();
  DLL_API virtual void PostSceneUpdate();

  DLL_API void SetSuspended(bool isSuspended);
  bool GetSuspended()
  {
    return mSuspended;
  }
  DLL_API void Draw();
  DLL_API void MouseUpdate(float x, float y, bool left, bool right, int wheel_delta);

 private:
  virtual void DefaultSceneInit() override;
  virtual void ResetSession() override;
  void SessionPrintStatus();

  void DrawWithOuterContext_DisplayDriverDelegate(int stage, DisplayDriverDelegateParams *params);
  bool UseOuterContext()
  {
    return mUseOuterContext;
  }
  ACQUIRE_LOCK bool EnableContextOpenGL();
  RELEASE_LOCK bool DisableContextOpenGL();
  void SetViewportOpenGL();

 private:
  bool mSuspended = false;
  bool mUseOuterContext = false;
  void *ptr_windowHWND = nullptr;
  void *ptr_hDC = nullptr;
  void *ptr_hRC = nullptr;
  bool mFrameFinished;
  void *mOpenGLContextLockPtr = nullptr;
  InteractiveCyclesOpenGLDisplayDriver *mDriverPtr;

  // Mouse data
  struct {
    float LastX = 0.0;
    float LastY = 0.0;
    int WheelDelta = 0;
    bool PrevLeft = false;
    bool PrevRight = false;
  } mMouse;
};

}  // namespace cycles_wrapper
