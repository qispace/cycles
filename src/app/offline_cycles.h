
#pragma once
#include "cycles_engine.h"


namespace cycles_wrapper {

class OfflineCycles : public CyclesEngine {

 public:
  DLL_API OfflineCycles();
  DLL_API virtual ~OfflineCycles();

  DLL_API bool RenderScene(const char *fileNameDest, bool useSharedMemory);
  DLL_API virtual bool SessionInit();
  DLL_API virtual bool SessionExit();
  DLL_API virtual void PostSceneUpdate();

  DLL_API void SetSamples(uint samples);
  DLL_API void SetIsSingleChannelFloat(bool value);

 private:
  virtual void DefaultSceneInit() override;
  virtual void ResetSession() override;
  void SessionPrintStatus();

 private:
  std::string mOutputFilepath;
  class OfflineCycles_OIIOOutputDriver *mOutputDriver;
};

}  // namespace cycles_wrapper
