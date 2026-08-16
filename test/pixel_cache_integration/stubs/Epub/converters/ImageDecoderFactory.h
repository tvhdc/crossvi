#pragma once

#include <string>

class GfxRenderer;

struct RenderConfig {
  int x = 0;
  int y = 0;
  int maxWidth = 0;
  int maxHeight = 0;
  bool useGrayscale = true;
  bool useDithering = true;
  bool performanceMode = false;
  bool useExactDimensions = false;
  bool cacheOnly = false;
  std::string cachePath;
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;
  virtual bool decodeToFramebuffer(const std::string&, GfxRenderer&, const RenderConfig&) = 0;
  virtual const char* getFormatName() const = 0;
};

class ImageDecoderFactory {
 public:
  static ImageToFramebufferDecoder* getDecoder(const std::string&) { return decoder; }

  inline static ImageToFramebufferDecoder* decoder = nullptr;
};
