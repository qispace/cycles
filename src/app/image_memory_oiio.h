/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#ifndef __IMAGE_OIIO__
#define __IMAGE_OIIO__

#include "scene/image.h"

namespace cycles_wrapper {

class OIIOImageMemoryLoader : public ccl::ImageLoader {
 public:
  OIIOImageMemoryLoader(const char *name,
                        const unsigned char *data,
                        size_t dataSize,
                        const char *mimeType,
                        bool compressAsSRGB = false);
  ~OIIOImageMemoryLoader();

  bool load_metadata(const ccl::ImageDeviceFeatures &features,
                     ccl::ImageMetaData &metadata) override;

  bool load_pixels(const ccl::ImageMetaData &metadata,
                   void *pixels,
                   const size_t pixels_size,
                   const bool associate_alpha) override;

  std::string name() const override;

  OpenImageIO_v2_4::ustring osl_filepath() const override;

  bool equals(const ImageLoader &other) const override;

 protected:
  const std::string mName;
  const unsigned char *mData;
  const size_t mDataSize;
  const std::string mMimeType;
  const bool mCompressAsSRGB;
};

}  // namespace cycles_wrapper

#endif /* __IMAGE_OIIO__ */
