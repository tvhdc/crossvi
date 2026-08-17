#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <stdint.h>

#include <cstdlib>
#include <cstring>
#include <string>

// Streaming cache writer for 2-bit pixels (4 levels). Packs 4 pixels per byte,
// MSB first.
//
// The .pxc file is written incrementally in small row bands rather than holding
// the whole decoded image in one heap buffer. A full-page image (e.g. 482x728)
// needs ~88KB packed, which will not fit alongside the ~20KB JPEG decoder on a
// fragmented 380KB heap (free heap is routinely ~55KB on an image page). When
// the cache cannot be written, every render pass re-decodes the JPEG from
// scratch; an anti-aliased image page renders ~14 times (BW + AA restore + two
// grayscale planes x ~6 strips), so a 2s decode becomes a ~30s freeze / watchdog
// reset. Streaming keeps the working set to a single MCU-row band, so caching
// succeeds and the image is decoded exactly once.
//
// Correctness relies on JPEGDEC delivering blocks in raster MCU order (outer
// loop over y, inner over x: see jpeg.inl DecodeJPEG). Consecutive MCU rows map
// to contiguous, non-overlapping destination row ranges, so once a block whose
// top row is Y arrives, every output row < Y is final and is flushed to disk.
struct PixelCache {
  uint8_t* buffer;   // band buffer: (bandRows + 1) rows; last row kept zeroed
  uint8_t* zeroRow;  // points at the spare zeroed row, for gap/clip fill
  int width;
  int height;
  int bytesPerRow;
  int originX;      // config.x - to convert screen coords to cache coords
  int originY;      // config.y
  int bandRows;     // rows held in the band buffer
  int bandStart;    // image-local row index of band buffer row 0
  int flushedRows;  // image-local rows already written to file
  HalFile file;
  std::string cachePathStr;
  std::string stagingPathStr;
  bool ok;

  PixelCache()
      : buffer(nullptr),
        zeroRow(nullptr),
        width(0),
        height(0),
        bytesPerRow(0),
        originX(0),
        originY(0),
        bandRows(0),
        bandStart(0),
        flushedRows(0),
        ok(false) {}
  PixelCache(const PixelCache&) = delete;
  PixelCache& operator=(const PixelCache&) = delete;

  static constexpr int MIN_BAND_ROWS = 16;
  static constexpr size_t MAX_BAND_BYTES = 24 * 1024;  // band working-set ceiling

  uint64_t expectedFileSize() const { return 4U + static_cast<uint64_t>(bytesPerRow) * static_cast<uint64_t>(height); }

  bool validateFile(const std::string& path) const {
    HalFile candidate;
    if (!Storage.openFileForRead("IMG", path, candidate)) return false;
    uint16_t storedWidth = 0;
    uint16_t storedHeight = 0;
    const bool valid = candidate.read(&storedWidth, sizeof(storedWidth)) == sizeof(storedWidth) &&
                       candidate.read(&storedHeight, sizeof(storedHeight)) == sizeof(storedHeight) &&
                       storedWidth == static_cast<uint16_t>(width) && storedHeight == static_cast<uint16_t>(height) &&
                       candidate.fileSize64() == expectedFileSize();
    return candidate.close() && valid;
  }

  bool publishStagingFile() {
    // Pixel caches are disposable derived data, and production reaches this
    // path only after the canonical cache failed validation. Validate and sync
    // the complete staging file first, then replace the unusable canonical in
    // two metadata operations. A power loss between them leaves a cache miss,
    // never user data loss; the next render regenerates it.
    const std::string legacyBackupPath = cachePathStr + ".bak";
    if (Storage.exists(legacyBackupPath.c_str()) && !Storage.remove(legacyBackupPath.c_str())) return false;
    if (Storage.exists(cachePathStr.c_str()) && !Storage.remove(cachePathStr.c_str())) return false;
    return Storage.rename(stagingPathStr.c_str(), cachePathStr.c_str());
  }

  // Open the cache file, write the header, and allocate a band buffer big enough
  // to hold the tallest single decode block (maxBlockDstRows output rows).
  bool begin(const std::string& cachePath, int w, int h, int ox, int oy, int maxBlockDstRows) {
    if (w <= 0 || h <= 0 || w > UINT16_MAX || h > UINT16_MAX || maxBlockDstRows <= 0) return false;
    width = w;
    height = h;
    originX = ox;
    originY = oy;
    bytesPerRow = (w + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
    bandStart = 0;
    flushedRows = 0;
    ok = false;
    cachePathStr = cachePath;
    stagingPathStr = cachePath + ".tmp";

    if (Storage.exists(stagingPathStr.c_str()) && !Storage.remove(stagingPathStr.c_str())) {
      LOG_ERR("IMG", "Failed to remove stale cache staging file: %s", stagingPathStr.c_str());
      return false;
    }

    int wantRows = maxBlockDstRows + 2;
    if (wantRows < MIN_BAND_ROWS) wantRows = MIN_BAND_ROWS;
    if (wantRows > h) wantRows = h;

    size_t maxRowsByMem = MAX_BAND_BYTES / (size_t)bytesPerRow;
    if (maxRowsByMem < 1) maxRowsByMem = 1;
    if ((size_t)wantRows > maxRowsByMem) wantRows = (int)maxRowsByMem;

    // A single decode block must fit inside the band, otherwise streaming would
    // drop rows. This only fails for pathological upscales that could not be
    // cached at all; fall back to the no-cache path.
    if (wantRows < maxBlockDstRows) {
      LOG_ERR("IMG", "Cache band too small (%d < %d rows) for %dx%d", wantRows, maxBlockDstRows, w, h);
      return false;
    }
    bandRows = wantRows;

    const size_t bufSize = (size_t)(bandRows + 1) * bytesPerRow;  // +1 spare zero row
    buffer = (uint8_t*)malloc(bufSize);
    if (!buffer) {
      LOG_ERR("IMG", "OOM cache band: %u bytes", (unsigned)bufSize);
      return false;
    }
    memset(buffer, 0, bufSize);
    zeroRow = buffer + (size_t)bandRows * bytesPerRow;

    if (!Storage.openFileForWrite("IMG", stagingPathStr, file)) {
      LOG_ERR("IMG", "Failed to open cache file for writing: %s", stagingPathStr.c_str());
      free(buffer);
      buffer = nullptr;
      return false;
    }

    uint16_t w16 = (uint16_t)w;
    uint16_t h16 = (uint16_t)h;
    if (file.write(&w16, 2) != 2 || file.write(&h16, 2) != 2) {
      LOG_ERR("IMG", "Failed to write cache header: %s", stagingPathStr.c_str());
      abort();
      return false;
    }

    LOG_DBG("IMG", "Cache stream started: %s (%dx%d, band %d rows)", cachePath.c_str(), w, h, bandRows);
    ok = true;
    return true;
  }

  // Flush every output row below newTopRow (they are final in raster order) and
  // reposition the band to start at newTopRow. Returns false if a write failed,
  // in which case the caller must stop caching for the rest of the decode.
  bool advanceTo(int newTopRow) {
    if (!ok) return false;
    if (newTopRow <= bandStart) return true;
    if (newTopRow > height) newTopRow = height;

    for (int r = bandStart; r < newTopRow; ++r) {
      const int idx = r - bandStart;
      const uint8_t* rowPtr = (idx < bandRows) ? (buffer + (size_t)idx * bytesPerRow) : zeroRow;
      if (file.write(rowPtr, (size_t)bytesPerRow) != (size_t)bytesPerRow) {
        LOG_ERR("IMG", "Cache write error at row %d", r);
        ok = false;
        return false;
      }
    }
    flushedRows = newTopRow;
    bandStart = newTopRow;
    memset(buffer, 0, (size_t)bandRows * bytesPerRow);  // fresh band (gaps stay black)
    return true;
  }

  // Flush the final band and zero-fill any rows never covered (image clipped by
  // the screen), then durably publish the complete staging file.
  bool finalize() {
    if (!ok) {
      abort();
      return false;
    }
    for (int r = flushedRows; r < height; ++r) {
      const int idx = r - bandStart;
      const uint8_t* rowPtr = (idx >= 0 && idx < bandRows) ? (buffer + (size_t)idx * bytesPerRow) : zeroRow;
      if (file.write(rowPtr, (size_t)bytesPerRow) != (size_t)bytesPerRow) {
        LOG_ERR("IMG", "Cache write error at row %d", r);
        abort();
        return false;
      }
    }
    const bool synced = file.sync();
    const bool closed = file.close();
    if (!synced || !closed || !validateFile(stagingPathStr) || !publishStagingFile()) {
      LOG_ERR("IMG", "Failed to publish cache: %s", cachePathStr.c_str());
      abort();
      return false;
    }
    LOG_DBG("IMG", "Cache written: %s (%dx%d, %d bytes)", cachePathStr.c_str(), width, height,
            4 + bytesPerRow * height);
    ok = false;  // file handed off; nothing left to clean up
    return true;
  }

  // Drop a partial/failed cache so a later decode re-creates it cleanly.
  void abort() {
    if (file.isOpen()) file.close();
    if (!stagingPathStr.empty() && Storage.exists(stagingPathStr.c_str())) {
      Storage.remove(stagingPathStr.c_str());
    }
    ok = false;
  }

  ~PixelCache() {
    if (file.isOpen()) {
      // The file is still open, so neither finalize() nor abort() ran, or a
      // mid-stream write failed (advanceTo() cleared ok but left the file open).
      // Drop the partial cache so we leave no corrupt file behind.
      abort();
    }
    if (buffer) {
      free(buffer);
      buffer = nullptr;
    }
  }
};
