#include "QrUtils.h"

#include <Utf8.h>
#include <qrcode.h>

#include <algorithm>
#include <memory>
#include <new>

#include "Logging.h"
#include "QrCapacity.h"

void QrUtils::drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload) {
  size_t len = textPayload.length();

  // Truncate to max QR capacity at a UTF-8 safe boundary to avoid splitting multi-byte sequences
  std::string truncated;
  const char* payload = textPayload.c_str();
  if (len > QrCapacity::maxBytes()) {
    len = utf8SafeTruncateBuffer(textPayload.c_str(), static_cast<int>(QrCapacity::maxBytes()));
    truncated = textPayload.substr(0, len);
    payload = truncated.c_str();
  }

  const QrCapacity::Selection selection = QrCapacity::select(len);
  if (len > selection.maxBytes) {
    LOG_ERR("QR", "Payload exceeds safe QR capacity: %u", static_cast<unsigned>(len));
    return;
  }
  const int version = selection.version;

  // Make sure we have a large enough buffer on the heap to avoid blowing the stack
  uint32_t bufferSize = qrcode_getBufferSize(version);
  auto qrcodeBytes = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[bufferSize]);
  if (!qrcodeBytes) {
    LOG_ERR("QR", "Unable to allocate QR buffer for version %d", version);
    return;
  }

  QRCode qrcode;
  // Initialize the QR code. We use ECC_LOW for max capacity.
  int8_t res = qrcode_initText(&qrcode, qrcodeBytes.get(), version, ECC_LOW, payload);

  if (res == 0) {
    // Determine the optimal pixel size.
    const int maxDim = std::min(bounds.width, bounds.height);

    int px = maxDim / qrcode.size;
    if (px < 1) px = 1;

    // Calculate centering X and Y
    const int qrDisplaySize = qrcode.size * px;
    const int xOff = bounds.x + (bounds.width - qrDisplaySize) / 2;
    const int yOff = bounds.y + (bounds.height - qrDisplaySize) / 2;

    // Draw the QR Code
    for (uint8_t cy = 0; cy < qrcode.size; cy++) {
      for (uint8_t cx = 0; cx < qrcode.size; cx++) {
        if (qrcode_getModule(&qrcode, cx, cy)) {
          renderer.fillRect(xOff + px * cx, yOff + px * cy, px, px, true);
        }
      }
    }
  } else {
    // If it fails (e.g. text too large), log an error
    LOG_ERR("QR", "Text too large for QR Code version %d", version);
  }
}
