#pragma once

#include <cstdint>

struct HalDisplay {
  enum RefreshMode { HALF_REFRESH };
};

class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  void setOrientation(Orientation) {}
  void displayBuffer() const {}
  void displayBuffer(HalDisplay::RefreshMode) const {}
  bool supportsStripGrayscale() const { return stripGrayscaleSupported; }
  int getDisplayHeight() const { return 80; }
  int getDisplayWidthBytes() const { return 2; }
  void beginStripTarget(uint8_t*, int, int) { beginStripCalls++; }
  void endStripTarget() { endStripCalls++; }
  void writeGrayscalePlaneStrip(bool lsb, const uint8_t*, int, int) {
    if (lsb)
      writeLsbStripCalls++;
    else
      writeMsbStripCalls++;
  }
  bool storeBwBuffer() {
    storeBwBufferCalls++;
    return storeBwBufferResult;
  }
  void clearScreen(uint8_t = 0xFF) { clearScreenCalls++; }
  void setRenderMode(RenderMode) { setRenderModeCalls++; }
  void copyGrayscaleLsbBuffers() { copyLsbCalls++; }
  void copyGrayscaleMsbBuffers() { copyMsbCalls++; }
  void displayGrayBuffer() { displayGrayCalls++; }
  void restoreBwBuffer() { restoreBwBufferCalls++; }
  void cleanupGrayscaleWithFrameBuffer() { cleanupGrayscaleCalls++; }

  bool stripGrayscaleSupported = true;
  bool storeBwBufferResult = true;
  int storeBwBufferCalls = 0;
  int clearScreenCalls = 0;
  int setRenderModeCalls = 0;
  int copyLsbCalls = 0;
  int copyMsbCalls = 0;
  int displayGrayCalls = 0;
  int restoreBwBufferCalls = 0;
  int beginStripCalls = 0;
  int endStripCalls = 0;
  int writeLsbStripCalls = 0;
  int writeMsbStripCalls = 0;
  int cleanupGrayscaleCalls = 0;
};
