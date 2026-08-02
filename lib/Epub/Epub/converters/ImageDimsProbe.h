#pragma once

#include <Print.h>

#include "ImageToFramebufferDecoder.h"

// Bounded streaming probe for JPEG/PNG dimensions.  The probe never buffers
// the image; it only keeps the parser state and the five bytes of a JPEG SOF.
// Returning a short write lets ZipFile stop inflation once the dimensions are
// known.
class ImageDimsProbe final : public Print {
 public:
  size_t write(uint8_t byte) override;
  size_t write(const uint8_t* data, size_t length) override;

  bool getDimensions(ImageDimensions& out) const;

 private:
  bool feed(uint8_t byte);

  enum class State : uint8_t {
    Sniff,
    PngHeader,
    JpegSoi,
    JpegFf,
    JpegMarker,
    JpegLenHi,
    JpegLenLo,
    JpegSkip,
    JpegSof,
    Done,
    Failed,
  };

  State state = State::Sniff;
  uint32_t bytesSeen = 0;
  uint32_t position = 0;
  static constexpr uint32_t MAX_PROBE_BYTES = 64 * 1024;
  uint32_t skipLeft = 0;
  uint16_t segmentLength = 0;
  bool sofPending = false;
  uint8_t sofBytes[5] = {0};
  uint8_t sofFill = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};
