#pragma once

#include <cstddef>
#include <cstdint>

#define JPEG_SCALE_EIGHTH 4
#define JPEG_SCALE_QUARTER 2
#define JPEG_SCALE_HALF 1

#define EIGHT_BIT_GRAYSCALE 1

#define JPEG_MODE_BASELINE 0
#define JPEG_MODE_PROGRESSIVE 1

struct JPEGFILE {
  void* fHandle = nullptr;
  int32_t iPos = 0;
  int32_t iSize = 0;
};

struct JPEGDRAW {
  void* pUser = nullptr;
  uint8_t* pPixels = nullptr;
  int x = 0;
  int y = 0;
  int iWidth = 0;
  int iHeight = 0;
  int iWidthUsed = 0;
};

using JPEG_DRAW_CALLBACK = int (*)(JPEGDRAW*);
using JPEG_OPEN_CALLBACK = void* (*)(const char*, int32_t*);
using JPEG_CLOSE_CALLBACK = void (*)(void*);
using JPEG_READ_CALLBACK = int32_t (*)(JPEGFILE*, uint8_t*, int32_t);
using JPEG_SEEK_CALLBACK = int32_t (*)(JPEGFILE*, int32_t);

class JPEGDEC {
 public:
  static void resetDecodeCalls() { decodeCalls_ = 0; }
  static size_t decodeCalls() { return decodeCalls_; }

  int open(const char* filename, JPEG_OPEN_CALLBACK openCallback, JPEG_CLOSE_CALLBACK closeCallback, JPEG_READ_CALLBACK,
           JPEG_SEEK_CALLBACK, JPEG_DRAW_CALLBACK drawCallback) {
    int32_t size = 0;
    void* handle = openCallback(filename, &size);
    if (!handle || size <= 0) return 0;
    closeCallback(handle);
    drawCallback_ = drawCallback;
    return 1;
  }

  void close() {}
  int getWidth() const { return 2; }
  int getHeight() const { return 1; }
  int getLastError() const { return 0; }
  int getJPEGType() const { return JPEG_MODE_BASELINE; }
  void setPixelType(int) {}
  void setUserPointer(void* user) { user_ = user; }

  int decode(int, int, int) {
    ++decodeCalls_;
    uint8_t pixels[2] = {0, 255};
    JPEGDRAW draw{user_, pixels, 0, 0, 2, 1, 2};
    return drawCallback_ && drawCallback_(&draw) == 1 ? 1 : 0;
  }

 private:
  inline static size_t decodeCalls_ = 0;
  JPEG_DRAW_CALLBACK drawCallback_ = nullptr;
  void* user_ = nullptr;
};
