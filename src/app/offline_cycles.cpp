#include "offline_cycles.h"

#include <stdio.h>

#include "device/device.h"
#include "scene/camera.h"
#include "scene/integrator.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "session/buffers.h"
#include "session/session.h"

#include "util/args.h"
#include "util/foreach.h"
#include "util/function.h"
#include "util/image.h"
#include "util/log.h"
#include "util/path.h"
#include "util/progress.h"
#include "util/string.h"
#include "util/time.h"
#include "util/transform.h"
#include "util/unique_ptr.h"
#include "util/version.h"

#include "app/cycles_xml.h"
#include "app/oiio_output_driver.h"

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>

using namespace ccl;
using namespace cycles_wrapper;

namespace cycles_wrapper {

class SharedMemoryImageOutput {
  /// unique_ptr to an ImageOutput.
  using unique_ptr = std::unique_ptr<SharedMemoryImageOutput>;

 public:
  SharedMemoryImageOutput()
  {
  }

  ~SharedMemoryImageOutput()
  {
    close();
  }

  void write(const ImageBuf &imgBuff)
  {
    int width = imgBuff.oriented_width();
    int height = imgBuff.oriented_height();
    int channels = imgBuff.nchannels();
    for (size_t i = 0; i < width; i++) {
      for (size_t j = 0; j < height; j++) {
        float rgba[4];
        imgBuff.getpixel(i, j, rgba, channels);
        auto pixelSize = sizeof(float) * channels;
        int offset = pixelSize * (i * height + j);
        memcpy_s((char *)(pView) + offset, pixelSize, rgba, pixelSize);
      }
    }
  }

  float *GetPixels()
  {
    return (float *)(pView);
  }

  void close()
  {
    // Unmap the memory-mapped file
    if (pView) {
      UnmapViewOfFile(pView);
      pView = nullptr;
    }
    // Close the handle
    if (hMapFile) {
      CloseHandle(hMapFile);
      hMapFile = nullptr;
    }
  }

  static unique_ptr create(const ccl::string_view filename)
  {
    auto retVal = std::make_unique<SharedMemoryImageOutput>();

    // Open the memory-mapped file for read/write access
    std::wstring filenameW(filename.begin(), filename.end());
    LPCWSTR memoryMapName = filenameW.c_str();
    retVal->hMapFile = OpenFileMapping(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, memoryMapName);
    if (retVal->hMapFile == nullptr) {
      // std::cerr << "Failed to open memory-mapped file." << std::endl;
      return nullptr;
    }

    // Map the memory-mapped file into the current process's address space
    retVal->pView = MapViewOfFile(retVal->hMapFile, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);

    if (retVal->pView == nullptr) {
      // std::cerr << "Failed to map memory-mapped file into address space." << std::endl;
      CloseHandle(retVal->hMapFile);
      retVal->hMapFile = nullptr;
      return nullptr;
    }

    return retVal;
  }

 private:
  HANDLE hMapFile = nullptr;
  LPVOID pView = nullptr;
};

class OfflineCycles_OIIOOutputDriver : public OIIOOutputDriver {
 public:
  OfflineCycles_OIIOOutputDriver(const ccl::string_view filepath,
                                 const ccl::string_view pass,
                                 OIIOOutputDriver::LogFunction log)
      : OIIOOutputDriver(filepath, pass, log), FlipHorizontally(false), IsSingleChannelFloat(false)
  {
  }

  void write_render_tile(const Tile &tile) override
  {
    /* Only write the full buffer, no intermediate tiles. */
    if (!(tile.size == tile.full_size)) {
      return;
    }

    log_(string_printf("OFFLINE_CYCLES_STATUS: Writing image %s", filepath_.c_str()));
    const int width = tile.size.x;
    const int height = tile.size.y;
    const int pixelCount = width * height;
    int channels = (IsSingleChannelFloat) ? 1 : 4;
    if (UseSharedMemory) {
      unique_ptr<SharedMemoryImageOutput> shared_memory_image_output;
      shared_memory_image_output = SharedMemoryImageOutput::create(filepath_);
      auto pixels = shared_memory_image_output->GetPixels();

      if (IsSingleChannelFloat) {
        vector<float> pixelsVec(width * height * 4);
        if (!tile.get_pass_pixels(pass_, 4, pixelsVec.data())) {
          log_("OFFLINE_CYCLES_STATUS: Failed to read render pass pixels");
          return;
        }
        for (size_t i = 0; i < pixelCount; i++) {
          pixels[i] = pixelsVec[i * 4 + 0];  // red channel
        }
      }
      else {  // not a single-channel float
        if (!tile.get_pass_pixels(pass_, channels, pixels)) {
          log_("OFFLINE_CYCLES_STATUS: Failed to read render pass pixels");
          return;
        }
        // Apply gamma correction for (some) non-linear file formats.
        if (ForceSrgbColorConversion) {
          const float g = 1.0f / 2.2f;
          const float gArray[] = {g, g, g, 1.0f};

          for (size_t i = 0; i < height; i++) {
            for (size_t j = 0; j < width; j++) {
              int pixelIndex = (i * width) + j;
              for (size_t k = 0; k < channels; k++) {
                float *pixelChannel = &pixels[pixelIndex * channels + k];
                *pixelChannel = std::pow(*pixelChannel, gArray[k]);
              }
            }
          }
        }
      }

      // Flip image vertically
      for (size_t i = 0; i < height / 2; i++) {
        for (size_t j = 0; j < width; j++) {
          int first = (i * width) + j;
          int last = ((height - i - 1) * width) + j;
          for (size_t k = 0; k < channels; k++) {
            std::swap(pixels[first * channels + k], pixels[last * channels + k]);
          }
        }
      }

      // Flip image horizontally to account for the difference in the coordinate system
      if (FlipHorizontally) {
        for (size_t i = 0; i < height; i++) {
          for (size_t j = 0; j < width / 2; j++) {
            int first = (i * width) + j;
            int last = (i * width) + (width - j - 1);
            for (size_t k = 0; k < channels; k++) {
              std::swap(pixels[first * channels + k], pixels[last * channels + k]);
            }
          }
        }
      }
    }
    else  // do not use shared memory, write to file
    {

      vector<float> pixels(width * height * 4);
      if (!tile.get_pass_pixels(pass_, 4, pixels.data())) {
        log_("OFFLINE_CYCLES_STATUS: Failed to read render pass pixels");
        return;
      }

      // Flip image horizontally to account for the difference in the coordinate system
      if (FlipHorizontally) {
        for (size_t i = 0; i < height; i++) {
          for (size_t j = 0; j < width / 2; j++) {
            int leftPixelIndex = (i * width) + j;
            int rightPixelIndex = (i * width) + (width - j - 1);
            std::swap(pixels[leftPixelIndex * 4 + 0], pixels[rightPixelIndex * 4 + 0]);  // R
            std::swap(pixels[leftPixelIndex * 4 + 1], pixels[rightPixelIndex * 4 + 1]);  // G
            std::swap(pixels[leftPixelIndex * 4 + 2], pixels[rightPixelIndex * 4 + 2]);  // B
            std::swap(pixels[leftPixelIndex * 4 + 3], pixels[rightPixelIndex * 4 + 3]);  // A
          }
        }
      }

      // Create the image file
      ImageSpec spec;
      if (IsSingleChannelFloat) {
        spec = ImageSpec(width, height, 1, TypeDesc::FLOAT);
      }
      else {
        spec = ImageSpec(width, height, 4, TypeDesc::FLOAT);
      }
      unique_ptr<SharedMemoryImageOutput> shared_memory_image_output;
      unique_ptr<ImageOutput> image_output = ImageOutput::create(filepath_);
      if (image_output == nullptr) {
        log_("OFFLINE_CYCLES_STATUS: Failed to create image file");
        return;
      }
      if (!image_output->open(filepath_, spec)) {
        log_("OFFLINE_CYCLES_STATUS: Failed to create image file");
        return;
      }
      const char *formatName = image_output->format_name();

      ImageBuf image_buffer;
      if (IsSingleChannelFloat) {
        vector<float> pixelsSingleChannel(width * height * 1);
        for (size_t i = 0; i < height; i++) {
          for (size_t j = 0; j < width; j++) {
            int pixelIndex = (i * width) + j;
            pixelsSingleChannel[pixelIndex] = pixels[pixelIndex * 4 + 0];  // R
          }
        }

        /* Manipulate offset and stride to convert from bottom-up to top-down convention. */
        image_buffer = ImageBuf(spec,
                                pixelsSingleChannel.data() + (height - 1) * width * 1,
                                AutoStride,
                                -width * 1 * sizeof(float),
                                AutoStride);
      }  // not a single-channel format
      else {
        /* Manipulate offset and stride to convert from bottom-up to top-down convention. */
        image_buffer = ImageBuf(spec,
                                pixels.data() + (height - 1) * width * 4,
                                AutoStride,
                                -width * 4 * sizeof(float),
                                AutoStride);

        /* Apply gamma correction for (some) non-linear file formats.
         * TODO: use OpenColorIO view transform if available. */
        if (ForceSrgbColorConversion ||
            ColorSpaceManager::detect_known_colorspace(u_colorspace_auto, "", formatName, true) ==
                u_colorspace_srgb) {
          const float g = 1.0f / 2.2f;
          ImageBufAlgo::pow(image_buffer, image_buffer, {g, g, g, 1.0f});
        }
      }

      // Write to disk and close
      TypeDesc format = TypeDesc::FLOAT;
      image_buffer.set_write_format(format);
      image_buffer.write(image_output.get());
      image_output->close();
    }
  }

  void UpdateFilePath(const ccl::string_view filepath, bool useSharedMemory)
  {
    filepath_ = filepath;
    UseSharedMemory = useSharedMemory;
  }

 public:
  bool FlipHorizontally;
  bool IsSingleChannelFloat;
  bool UseSharedMemory;
  bool ForceSrgbColorConversion = false;
};

}  // namespace cycles_wrapper

OfflineCycles::OfflineCycles() : CyclesEngine()
{
  // IO
  mOutputFilepath = "result.png";

  /* device names */
  string device_names = "";
  string devicename = "CPU";

  /* List devices for which support is compiled in. */
  vector<DeviceType> types = Device::available_types();
  foreach (DeviceType type, types) {
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
  mOptions.session_params->background = true;
  mOptions.interactive = false;
  //mOptions.session_params->threads = 32;

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

OfflineCycles::~OfflineCycles()
{
}

void OfflineCycles::SessionPrintStatus()
{
  if (!mOptions.session)
    return;

  /* get status */
  int sample = mOptions.session->progress.get_current_sample();
  double progress = mOptions.session->progress.get_progress();
  string status, substatus;
  mOptions.session->progress.get_status(status, substatus);

  if (substatus != "")
    status += ": " + substatus;

  /* print status */
  status = string_printf(
      "OFFLINE_CYCLES_STATUS: Progress %05.2f   %s", (double)progress * 100, status.c_str());
  Log(LOG_TYPE_DEBUG, status);

  mCurrentSample = sample;
}

bool OfflineCycles::SessionInit()
{
  bool isOk = CyclesEngine::SessionInit();
  mOptions.output_pass = "combined";
  mOptions.session = std::make_unique<Session>(*mOptions.session_params, *mOptions.scene_params);

  mOptions.session->scene->integrator->set_use_denoise(true);
  mOptions.session->scene->integrator->set_denoiser_prefilter(ccl::DenoiserPrefilter::DENOISER_PREFILTER_NONE);
  mOptions.session->scene->integrator->tag_modified();

  if (!mOutputFilepath.empty()) {
    std::unique_ptr<OIIOOutputDriver> driver = std::make_unique<OfflineCycles_OIIOOutputDriver>(
        mOutputFilepath, mOptions.output_pass, [this](const std::string &str) {
          this->Log(LOG_TYPE_INFO, str);
        });
    mOutputDriver = (OfflineCycles_OIIOOutputDriver *)driver.get();
    mOptions.session->set_output_driver(std::move(driver));
  }

  if (mOptions.session_params->background && !mOptions.quiet)
    mOptions.session->progress.set_update_callback([this]() { this->SessionPrintStatus(); });

  // Load scene
  DefaultSceneInit();

  // Add pass for output.
  Pass *pass = mOptions.session->scene->create_node<Pass>();
  pass->set_name(ustring(mOptions.output_pass.c_str()));
  pass->set_type(PASS_COMBINED);

  return isOk;
}

bool OfflineCycles::SessionExit()
{
  bool isOk = CyclesEngine::SessionExit();
  mOptions.session.reset();

  if (mOptions.session_params->background && !mOptions.quiet) {
    Log(LOG_TYPE_INFO, "OFFLINE_CYCLES_STATUS: Finished");
    printf("\n");
  }
  return isOk;
}

void OfflineCycles::PostSceneUpdate()
{
  CyclesEngine::PostSceneUpdate();

  // Reset the scene
  mOptions.session->scene->reset();
  mOptions.session->scene->default_background = mNameToShader[mCurrentBackgroundShaderName];

  // Start the session
  ResetSession();
  mOptions.session->start();
}

void OfflineCycles::SetSamples(uint samples)
{
  assert(samples > 0);
  mOptions.session_params->samples = samples;
}

void OfflineCycles::SetIsSingleChannelFloat(bool value)
{
  mOutputDriver->IsSingleChannelFloat = value;
}

void OfflineCycles::DefaultSceneInit()
{
  CyclesEngine::DefaultSceneInit();

  ccl::Scene *scene = mOptions.session->scene;

  /* Camera width/height override? */
  if (!(mOptions.width == 0 || mOptions.height == 0)) {
    scene->camera->set_full_width(mOptions.width);
    scene->camera->set_full_height(mOptions.height);
  }
  else {
    mOptions.width = scene->camera->get_full_width();
    mOptions.height = scene->camera->get_full_height();
  }

  /* Calculate Viewplane */
  scene->camera->compute_auto_viewplane();
}

void OfflineCycles::ResetSession()
{
  CyclesEngine::ResetSession();
}

bool OfflineCycles::RenderScene(const char *fileNameDest, bool useSharedMemory)
{
  // Coordinate system-based correction is not needed for panoramic for some reason
  mOutputDriver->FlipHorizontally = mOptions.session->scene->camera->get_camera_type() !=
                                    CameraType::Panoramic;

  mOutputDriver->UpdateFilePath(fileNameDest, useSharedMemory);
  path_init();
  ResetSession();
  mOptions.session->start();

  // ...

  mOptions.session->wait();

  return true;
}
