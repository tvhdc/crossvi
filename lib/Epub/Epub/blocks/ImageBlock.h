#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class BoundedFileReader;
namespace serialization {
class BufferedFileWriter;
}

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);
  ImageBlock(const std::string& imagePath, const std::string& sourcePath, int16_t width, int16_t height);
  ~ImageBlock() override;

  const std::string& getImagePath() const { return imagePath; }
  const std::string& getSourcePath() const { return sourcePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;
  bool hasValidCache() const;
  bool needsDecode() const;
  bool preparePixelCache(GfxRenderer& renderer, int x, int y) const;
  bool wasDecodedWithoutCache() const { return decodedWithoutCache; }
  void renderPlaceholder(GfxRenderer& renderer, int x, int y) const;
  static void clearSessionRenderFailures();

  using ExtractFn = bool (*)(void* context, const char* sourcePath, const char* destinationPath);
  static void setExtractor(void* context, ExtractFn extractor);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  void render(GfxRenderer& renderer, int x, int y, std::unique_ptr<uint8_t[]>& readBuffer, size_t& readBufferCapacity);
  bool serialize(serialization::BufferedFileWriter& file);
  static std::unique_ptr<ImageBlock> deserialize(BoundedFileReader& reader);

 private:
  std::string imagePath;
  std::string sourcePath;
  mutable std::string pixelCachePath;
  std::unique_ptr<uint8_t[]> residentPixels;
  size_t residentPixelBytes = 0;
  uint16_t residentWidth = 0;
  uint16_t residentHeight = 0;
  bool decodedWithoutCache = false;
  bool renderFailed = false;
  int16_t width;
  int16_t height;

  const std::string& getPixelCachePath() const;

  static void* extractContext;
  static ExtractFn extractFn;
};
