#include "DictZip.h"

#include <Arduino.h>
#include <InflateReader.h>
#include <Memory.h>

#include <algorithm>
#include <climits>
#include <cstddef>

namespace DictZip {
namespace {

// Caps the chunk table at 32KB of heap (8192 * 4 bytes); at the typical ~58KB
// chunk length that still allows ~460MB of uncompressed dictionary data.
constexpr uint16_t MAX_CHUNK_COUNT = 8192;
constexpr size_t CHUNK_TABLE_HEAP_HEADROOM_BYTES = 1024;

bool readLe16(HalFile& file, uint16_t* out) {
  uint8_t raw[2];
  if (file.read(raw, 2) != 2) return false;
  *out = static_cast<uint16_t>(raw[0] | (static_cast<uint16_t>(raw[1]) << 8));
  return true;
}

constexpr size_t INPUT_BUF_BYTES = 2048;

struct ChunkSource {
  InflateReader reader;  // must stay first: uzlib passes only its embedded state
  HalFile* file = nullptr;
  uint32_t remaining = 0;
  bool readFailed = false;
  uint8_t buf[INPUT_BUF_BYTES] = {};
};

static_assert(offsetof(ChunkSource, reader) == 0);

int chunkReadCb(uzlib_uncomp* u) {
  auto* source = reinterpret_cast<ChunkSource*>(u);
  if (source->remaining == 0) return -1;

  const uint32_t wanted = std::min<uint32_t>(source->remaining, INPUT_BUF_BYTES);
  const int read = source->file->read(source->buf, static_cast<int>(wanted));
  if (read <= 0) {
    source->readFailed = true;
    return -1;
  }
  source->remaining -= static_cast<uint32_t>(read);
  u->source = source->buf + 1;
  u->source_limit = source->buf + read;
  return source->buf[0];
}

bool extractChunkSlice(HalFile& file, uint32_t compressedOffset, uint32_t compressedSize, uint32_t discardSize,
                       uint32_t extractSize, HalFile& outFile, ExtractError* outError) {
  const auto fail = [outError](const ExtractError error) {
    if (outError) *outError = error;
    return false;
  };
  if (extractSize == 0) return true;

  // Reserve the largest block first; compressed input is then streamed through
  // a fixed 2 KiB buffer instead of keeping a whole dictzip chunk in RAM.
  auto ring = makeUniqueNoThrow<uint8_t[]>(InflateReader::RING_BYTES);
  if (!ring) return fail(ExtractError::LowMemory);

  auto source = makeUniqueNoThrow<ChunkSource>();
  if (!source) return fail(ExtractError::LowMemory);
  source->file = &file;
  source->remaining = compressedSize;

  if (!file.seekSet(compressedOffset)) return fail(ExtractError::ReadError);
  if (!source->reader.initWithRing(ring.get())) return fail(ExtractError::LowMemory);
  source->reader.setReadCallback(&chunkReadCb);

  auto buf = makeUniqueNoThrow<uint8_t[]>(512);
  if (!buf) return fail(ExtractError::LowMemory);

  const auto decodeFailed = [&source, &fail] {
    return fail(source->readFailed ? ExtractError::ReadError : ExtractError::Decompress);
  };

  uint32_t batch;
  while (discardSize > 0) {
    batch = discardSize < 512 ? discardSize : 512;
    if (!source->reader.read(buf.get(), batch)) return decodeFailed();
    discardSize -= batch;
  }

  while (extractSize > 0) {
    batch = extractSize < 512 ? extractSize : 512;
    if (!source->reader.read(buf.get(), batch)) return decodeFailed();
    if (outFile.write(buf.get(), batch) != batch) return fail(ExtractError::ReadError);
    extractSize -= batch;
  }

  return true;
}

}  // namespace

bool parse(HalFile& file, Info* info, ExtractError* outError) {
  const auto fail = [outError](const ExtractError error) {
    if (outError) *outError = error;
    return false;
  };
  if (outError) *outError = ExtractError::None;
  if (!info) return fail(ExtractError::Decompress);
  *info = {};

  uint8_t header[10];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return fail(ExtractError::ReadError);
  if (header[0] != 0x1f || header[1] != 0x8b || header[2] != 8) return fail(ExtractError::Decompress);

  const uint8_t flags = header[3];
  if ((flags & 0x04) == 0) return fail(ExtractError::Decompress);  // dictzip requires FEXTRA

  uint16_t xlen = 0;
  if (!readLe16(file, &xlen)) return fail(ExtractError::ReadError);

  uint32_t extraRead = 0;
  bool foundRa = false;
  while (extraRead + 4 <= xlen) {
    uint8_t subHeader[4];
    if (file.read(subHeader, sizeof(subHeader)) != static_cast<int>(sizeof(subHeader))) {
      return fail(ExtractError::ReadError);
    }
    extraRead += 4;
    const uint16_t subLen = static_cast<uint16_t>(subHeader[2] | (static_cast<uint16_t>(subHeader[3]) << 8));
    if (extraRead + subLen > xlen) return fail(ExtractError::Decompress);

    if (subHeader[0] == 'R' && subHeader[1] == 'A') {
      if (foundRa || subLen < 6) return fail(ExtractError::Decompress);

      uint16_t version = 0;
      uint16_t chunkLen = 0;
      uint16_t chunkCount = 0;
      if (!readLe16(file, &version) || !readLe16(file, &chunkLen) || !readLe16(file, &chunkCount)) {
        return fail(ExtractError::ReadError);
      }
      extraRead += 6;
      if (version != 1 || chunkLen == 0 || chunkCount == 0 || chunkCount > MAX_CHUNK_COUNT) {
        return fail(ExtractError::Decompress);
      }
      if (subLen != static_cast<uint16_t>(6 + chunkCount * 2)) return fail(ExtractError::Decompress);

      info->chunkLength = chunkLen;
      const size_t chunkTableBytes = (static_cast<size_t>(chunkCount) + 1) * sizeof(uint32_t);
      if (ESP.getMaxAllocHeap() < chunkTableBytes + CHUNK_TABLE_HEAP_HEADROOM_BYTES) {
        return fail(ExtractError::LowMemory);
      }
      info->chunkOffsets.reserve(static_cast<size_t>(chunkCount) + 1);
      info->chunkOffsets.push_back(0);
      uint32_t cumulative = 0;
      for (uint16_t i = 0; i < chunkCount; i++) {
        uint16_t compLen = 0;
        if (!readLe16(file, &compLen)) return fail(ExtractError::ReadError);
        extraRead += 2;
        cumulative += compLen;
        info->chunkOffsets.push_back(cumulative);
      }
      foundRa = true;
    } else {
      if (!file.seekSet(file.position() + subLen)) return fail(ExtractError::ReadError);
      extraRead += subLen;
    }
  }
  if (extraRead != xlen || !foundRa) return fail(ExtractError::Decompress);

  if (flags & 0x08) {  // FNAME
    int b;
    do {
      b = file.read();
      if (b < 0) return fail(ExtractError::ReadError);
    } while (b != 0);
  }
  if (flags & 0x10) {  // FCOMMENT
    int b;
    do {
      b = file.read();
      if (b < 0) return fail(ExtractError::ReadError);
    } while (b != 0);
  }
  if (flags & 0x02) {  // FHCRC
    uint8_t crc[2];
    if (file.read(crc, 2) != 2) return fail(ExtractError::ReadError);
  }

  const uint64_t fileSize64 = file.fileSize64();
  const uint64_t dataOffset64 = file.position();
  if (fileSize64 > UINT32_MAX || dataOffset64 > UINT32_MAX || fileSize64 < dataOffset64 + 8) {
    return fail(ExtractError::Decompress);
  }
  const uint32_t fileSize = static_cast<uint32_t>(fileSize64);
  info->dataOffset = static_cast<uint32_t>(dataOffset64);
  if (info->chunkOffsets.back() > fileSize - info->dataOffset - 8) return fail(ExtractError::Decompress);
  if (!file.seekSet(fileSize - 4)) return fail(ExtractError::ReadError);
  uint8_t isizeRaw[4];
  if (file.read(isizeRaw, 4) != 4) return fail(ExtractError::ReadError);
  info->totalSize = static_cast<uint32_t>(isizeRaw[0]) | (static_cast<uint32_t>(isizeRaw[1]) << 8) |
                    (static_cast<uint32_t>(isizeRaw[2]) << 16) | (static_cast<uint32_t>(isizeRaw[3]) << 24);
  if (info->totalSize == 0) return fail(ExtractError::Decompress);
  info->valid = true;
  return true;
}

bool extractEntry(const char* path, uint32_t offset, uint32_t size, HalFile& outFile, ExtractError* outError) {
  const auto fail = [outError](const ExtractError error) {
    if (outError) *outError = error;
    return false;
  };
  if (outError) *outError = ExtractError::None;
  if (size == 0) return true;

  HalFile file;
  if (!Storage.openFileForRead("DICTZIP", path, file)) return fail(ExtractError::ReadError);

  Info info;
  if (!parse(file, &info, outError)) return false;

  // Reject ranges outside the uncompressed data (offset/size come from the
  // untrusted .idx). Subtraction form avoids uint32 overflow in offset + size
  // and guarantees localOffset < chunkOutSize in the loop below.
  if (offset > info.totalSize || size > info.totalSize - offset) return fail(ExtractError::ReadError);

  const uint32_t startChunk = offset / info.chunkLength;
  const uint32_t endChunk = (offset + size - 1) / info.chunkLength;
  if (endChunk + 1 >= info.chunkOffsets.size()) return fail(ExtractError::ReadError);

  uint32_t remaining = size;
  const uint32_t lastChunk = static_cast<uint32_t>(info.chunkOffsets.size() - 2);
  for (uint32_t chunk = startChunk; chunk <= endChunk; chunk++) {
    uint32_t chunkOutSize = info.chunkLength;
    if (chunk == lastChunk) chunkOutSize = info.totalSize - chunk * info.chunkLength;
    if (chunkOutSize == 0 || chunkOutSize > info.chunkLength) chunkOutSize = info.chunkLength;

    const uint32_t localOffset = (chunk == startChunk) ? (offset % info.chunkLength) : 0;
    const uint32_t available = chunkOutSize - localOffset;
    const uint32_t take = remaining < available ? remaining : available;

    const uint32_t compOffset = info.dataOffset + info.chunkOffsets[chunk];
    const uint32_t compSize = info.chunkOffsets[chunk + 1] - info.chunkOffsets[chunk];
    if (!extractChunkSlice(file, compOffset, compSize, localOffset, take, outFile, outError)) return false;

    remaining -= take;
    if (remaining == 0) break;
  }

  if (remaining != 0) return fail(ExtractError::Decompress);
  return true;
}

}  // namespace DictZip
