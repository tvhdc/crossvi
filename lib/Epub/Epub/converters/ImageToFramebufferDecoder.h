#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

class GfxRenderer;

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  bool useDithering = true;
  bool performanceMode = false;
  bool useExactDimensions = false;  // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  bool preserveAlpha = false;       // If true, alpha 0 leaves framebuffer pixels unchanged.
  bool writeWhiteInBw = false;      // If true, opaque white pixels erase existing BW pixels.
  std::string cachePath;            // If non-empty, decoder will write pixel cache to this path
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

 protected:
  // Size validation helpers
  static constexpr int64_t MAX_SOURCE_DIMENSION = INT16_MAX;
  static constexpr int MAX_SOURCE_PIXELS = 3145728;  // 2048 * 1536

  static bool validateImageDimensions(int width, int height, const std::string& format);
  static bool validateAndStoreDimensions(int64_t width, int64_t height, ImageDimensions& out, const char* format);
  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
