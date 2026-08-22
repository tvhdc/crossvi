#include <HalDisplay.h>
#include <HalGPIO.h>
#include <Logging.h>

// Global HalDisplay instance
HalDisplay display;

#define SD_SPI_MISO 7

namespace {
#ifdef ENABLE_SERIAL_LOG
uint32_t nextRefreshTraceId() {
  static uint32_t id = 0;
  return ++id;
}
#endif

const char* halRefreshModeName(const HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return "FULL";
    case HalDisplay::HALF_REFRESH:
      return "HALF";
    case HalDisplay::FAST_REFRESH:
    default:
      return "FAST";
  }
}
}  // namespace

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin(bool seamless) {
  const auto wakeupReason = gpio.getWakeupReason();
  LOG_DBG("EPD", "hal_begin seamless=%u x3=%u wake=%u", static_cast<unsigned>(seamless),
          static_cast<unsigned>(gpio.deviceIsX3()), static_cast<unsigned>(wakeupReason));

  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  einkDisplay.begin();

  if (seamless) {
    // Defuse the SDK's X3 _x3InitialFullSyncsRemaining counter (no-op on X4)
    // so the first paint isn't promoted to FULL (~770ms). Skips the wakeup-
    // gated requestResync() below for the same reason.
    einkDisplay.skipInitialResync();
    return;
  }
  // Request resync after specific wakeup events to ensure clean display state.
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    // The retained sleep frame can remain visible through parts of the first
    // X3 wake paint. One post-condition pass settles the newly displayed frame
    // before normal differential updates resume. Other controllers ignore the
    // pass count and keep their existing resync behavior.
    LOG_DBG("EPD", "hal_begin request_resync passes=1 wake=%u", static_cast<unsigned>(wakeupReason));
    einkDisplay.requestResync(1);
  }
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  const bool x3HalfResync = gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH;
#ifdef ENABLE_SERIAL_LOG
  const uint32_t refreshId = nextRefreshTraceId();
  const unsigned long startedAt = millis();
  LOG_DBG("EPD", "refresh_id=%lu op=display begin req=%s off=%u x3=%u x3_half_resync=%u",
          static_cast<unsigned long>(refreshId), halRefreshModeName(mode), static_cast<unsigned>(turnOffScreen),
          static_cast<unsigned>(gpio.deviceIsX3()), static_cast<unsigned>(x3HalfResync));
#endif
  if (x3HalfResync) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
#ifdef ENABLE_SERIAL_LOG
  LOG_DBG("EPD", "refresh_id=%lu op=display complete elapsed_ms=%lu", static_cast<unsigned long>(refreshId),
          millis() - startedAt);
#endif
}

void HalDisplay::triggerDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
#ifdef ENABLE_SERIAL_LOG
  const uint32_t refreshId = nextRefreshTraceId();
  LOG_DBG("EPD", "refresh_id=%lu op=trigger begin req=%s off=%u", static_cast<unsigned long>(refreshId),
          halRefreshModeName(mode), static_cast<unsigned>(turnOffScreen));
#endif
  einkDisplay.triggerDisplay(convertRefreshMode(mode), turnOffScreen);
#ifdef ENABLE_SERIAL_LOG
  LOG_DBG("EPD", "refresh_id=%lu op=trigger dispatched", static_cast<unsigned long>(refreshId));
#endif
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  const bool x3HalfResync = gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH;
#ifdef ENABLE_SERIAL_LOG
  const uint32_t refreshId = nextRefreshTraceId();
  const unsigned long startedAt = millis();
  LOG_DBG("EPD", "refresh_id=%lu op=refresh begin req=%s off=%u x3=%u x3_half_resync=%u",
          static_cast<unsigned long>(refreshId), halRefreshModeName(mode), static_cast<unsigned>(turnOffScreen),
          static_cast<unsigned>(gpio.deviceIsX3()), static_cast<unsigned>(x3HalfResync));
#endif
  if (x3HalfResync) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
#ifdef ENABLE_SERIAL_LOG
  LOG_DBG("EPD", "refresh_id=%lu op=refresh complete elapsed_ms=%lu", static_cast<unsigned long>(refreshId),
          millis() - startedAt);
#endif
}

void HalDisplay::requestResync(const uint8_t settlePasses) { einkDisplay.requestResync(settlePasses); }

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t* sizeOut) { return einkDisplay.lendBuildStorage(sizeOut); }

void HalDisplay::returnFrameBufferStorage() { einkDisplay.returnBuildStorage(); }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
#ifdef ENABLE_SERIAL_LOG
  const uint32_t refreshId = nextRefreshTraceId();
  const unsigned long startedAt = millis();
  LOG_DBG("EPD", "refresh_id=%lu op=gray-base begin fallback=%s off=%u x3=%u",
          static_cast<unsigned long>(refreshId), halRefreshModeName(fallback), static_cast<unsigned>(turnOffScreen),
          static_cast<unsigned>(gpio.deviceIsX3()));
#endif
  // X3: a HALF fallback means the caller wants a clean base (e.g. the sleep
  // cover, a full-screen swap from arbitrary prior content). Without this, the
  // X3 grayscale base takes its gentle differential happy path and the prior
  // home/reader frame ghosts through the soft aa_pre_bw_mid waveform. Forcing a
  // resync makes displayGrayscaleBase clear first, matching displayBuffer(HALF).
  // The reader's FAST path is deliberately left on the differential path so
  // per-page grayscale stays cheap.
  if (gpio.deviceIsX3() && fallback == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
#ifdef ENABLE_SERIAL_LOG
  LOG_DBG("EPD", "refresh_id=%lu op=gray-base complete elapsed_ms=%lu", static_cast<unsigned long>(refreshId),
          millis() - startedAt);
#endif
}

void HalDisplay::preconditionGrayscale() { einkDisplay.preconditionGrayscale(); }

void HalDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  einkDisplay.preconditionGrayscale(x, y, w, h);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
#ifdef ENABLE_SERIAL_LOG
  const uint32_t refreshId = nextRefreshTraceId();
  const unsigned long startedAt = millis();
  LOG_DBG("EPD", "refresh_id=%lu op=gray-planes begin off=%u x3=%u", static_cast<unsigned long>(refreshId),
          static_cast<unsigned>(turnOffScreen), static_cast<unsigned>(gpio.deviceIsX3()));
#endif
  einkDisplay.displayGrayBuffer(turnOffScreen);
#ifdef ENABLE_SERIAL_LOG
  LOG_DBG("EPD", "refresh_id=%lu op=gray-planes complete elapsed_ms=%lu", static_cast<unsigned long>(refreshId),
          millis() - startedAt);
#endif
}

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

bool HalDisplay::supportsStripGrayscale() const { return einkDisplay.supportsStripGrayscale(); }

bool HalDisplay::supportsX3GhostCleanup() const { return einkDisplay.supportsFastLutProfiles(); }

bool HalDisplay::cleanX3GhostingNow() { return einkDisplay.cleanFastGhosting(); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
