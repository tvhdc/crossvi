#include "SleepFrameStore.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <StagedFileTransaction.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace {
constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";
constexpr char SLEEP_FRAME_TEMP_FILE[] = "/.crosspoint/sleep_frame.bin.tmp";
constexpr char SLEEP_FRAME_BACKUP_FILE[] = "/.crosspoint/sleep_frame.bin.bak";
constexpr uint64_t X3_SLEEP_FRAME_BYTES = 792ULL * 528ULL / 8ULL;
constexpr uint64_t X4_SLEEP_FRAME_BYTES = 800ULL * 480ULL / 8ULL;
constexpr std::array<uint8_t, 4> SLEEP_FRAME_MAGIC = {'C', 'V', 'S', 'F'};
constexpr uint8_t SLEEP_FRAME_VERSION = 1;
constexpr size_t SLEEP_FRAME_HEADER_SIZE = 24;

struct SleepFrameSpec {
  uint8_t model = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t payloadSize = 0;
};

uint16_t readU16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8U;
}

uint32_t readU32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8U |
         static_cast<uint32_t>(bytes[2]) << 16U | static_cast<uint32_t>(bytes[3]) << 24U;
}

void writeU16(uint8_t* bytes, const uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
}

void writeU32(uint8_t* bytes, const uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
  bytes[2] = static_cast<uint8_t>(value >> 16U);
  bytes[3] = static_cast<uint8_t>(value >> 24U);
}

bool frameSpec(const uint16_t width, const uint16_t height, SleepFrameSpec& out) {
  if (width == 528 && height == 792) {
    out = {3, width, height, static_cast<uint32_t>(X3_SLEEP_FRAME_BYTES)};
    return true;
  }
  if (width == 480 && height == 800) {
    out = {4, width, height, static_cast<uint32_t>(X4_SLEEP_FRAME_BYTES)};
    return true;
  }
  return false;
}

std::array<uint8_t, SLEEP_FRAME_HEADER_SIZE> encodeHeader(const SleepFrameSpec& spec, const uint32_t payloadHash) {
  std::array<uint8_t, SLEEP_FRAME_HEADER_SIZE> header{};
  std::copy(SLEEP_FRAME_MAGIC.begin(), SLEEP_FRAME_MAGIC.end(), header.begin());
  header[4] = SLEEP_FRAME_VERSION;
  header[5] = spec.model;
  writeU16(header.data() + 8, spec.width);
  writeU16(header.data() + 10, spec.height);
  writeU32(header.data() + 12, spec.payloadSize);
  writeU32(header.data() + 16, payloadHash);
  return header;
}

bool decodeHeader(const uint8_t* header, const SleepFrameSpec& expected, uint32_t& payloadHash) {
  if (!std::equal(SLEEP_FRAME_MAGIC.begin(), SLEEP_FRAME_MAGIC.end(), header) ||
      header[4] != SLEEP_FRAME_VERSION || header[5] != expected.model || header[6] != 0 || header[7] != 0 ||
      readU16(header + 8) != expected.width || readU16(header + 10) != expected.height ||
      readU32(header + 12) != expected.payloadSize || readU32(header + 20) != 0) {
    return false;
  }
  payloadHash = readU32(header + 16);
  return true;
}

bool validateSleepFrameFile(const char* path, void* context) {
  const auto& expected = *static_cast<const SleepFrameSpec*>(context);
  HalFile file;
  if (!Storage.openFileForRead("SLP", path, file)) return false;
  if (file.fileSize64() != SLEEP_FRAME_HEADER_SIZE + expected.payloadSize) return file.close() && false;
  std::array<uint8_t, SLEEP_FRAME_HEADER_SIZE> header{};
  uint32_t expectedHash = 0;
  if (file.read(header.data(), header.size()) != static_cast<int>(header.size()) ||
      !decodeHeader(header.data(), expected, expectedHash)) {
    return file.close() && false;
  }
  StagedFileTransaction::Digest payloadDigest;
  std::array<uint8_t, 2048> bytes{};
  uint32_t remaining = expected.payloadSize;
  while (remaining > 0) {
    const size_t wanted = std::min<size_t>(bytes.size(), remaining);
    if (file.read(bytes.data(), wanted) != static_cast<int>(wanted)) return file.close() && false;
    StagedFileTransaction::updateDigest(payloadDigest, bytes.data(), wanted);
    remaining -= static_cast<uint32_t>(wanted);
  }
  return file.close() && payloadDigest.size == expected.payloadSize && payloadDigest.hash == expectedHash;
}
}  // namespace

namespace SleepFrameStore {

void discard() {
  Storage.remove(SLEEP_FRAME_TEMP_FILE);
  Storage.remove(SLEEP_FRAME_BACKUP_FILE);
  Storage.remove(SLEEP_FRAME_FILE);
}

bool save(const GfxRenderer& renderer) {
  const uint8_t* buffer = renderer.getFrameBuffer();
  const size_t bufferSize = renderer.getBufferSize();
  SleepFrameSpec spec;
  if (!buffer || !frameSpec(renderer.getScreenWidth(), renderer.getScreenHeight(), spec) ||
      bufferSize != spec.payloadSize) {
    Storage.remove(SLEEP_FRAME_TEMP_FILE);
    return false;
  }

  StagedFileTransaction::Digest payloadDigest;
  StagedFileTransaction::updateDigest(payloadDigest, buffer, bufferSize);
  const auto header = encodeHeader(spec, payloadDigest.hash);

  Storage.remove(SLEEP_FRAME_TEMP_FILE);
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_TEMP_FILE, file)) return false;
  const size_t headerWritten = file.write(header.data(), header.size());
  const size_t payloadWritten = headerWritten == header.size() ? file.write(buffer, bufferSize) : 0;
  bool saved = headerWritten == header.size() && payloadWritten == bufferSize;
  saved = file.sync() && saved;
  saved = file.close() && saved;
  if (!saved) {
    Storage.remove(SLEEP_FRAME_TEMP_FILE);
    return false;
  }

  StagedFileTransaction::Digest digest;
  StagedFileTransaction::updateDigest(digest, header.data(), header.size());
  StagedFileTransaction::updateDigest(digest, buffer, payloadWritten);
  const auto status = StagedFileTransaction::publishAndVerify(
      SLEEP_FRAME_FILE, SLEEP_FRAME_TEMP_FILE, SLEEP_FRAME_BACKUP_FILE, digest, validateSleepFrameFile, &spec);
  if (status != StagedFileTransaction::Status::Published) Storage.remove(SLEEP_FRAME_TEMP_FILE);
  return status == StagedFileTransaction::Status::Published;
}

bool ready(const bool deviceIsX3) {
  SleepFrameSpec expected;
  frameSpec(deviceIsX3 ? 528 : 480, deviceIsX3 ? 792 : 800, expected);
  if (StagedFileTransaction::recover(SLEEP_FRAME_FILE, SLEEP_FRAME_BACKUP_FILE, validateSleepFrameFile,
                                     &expected) == StagedFileTransaction::Status::IoError) {
    return false;
  }
  if (validateSleepFrameFile(SLEEP_FRAME_FILE, &expected)) return true;
  discard();
  return false;
}

bool load(HalDisplay& display, const bool consume) {
  SleepFrameSpec expected;
  if (!frameSpec(display.getDisplayWidth(), display.getDisplayHeight(), expected) ||
      StagedFileTransaction::recover(SLEEP_FRAME_FILE, SLEEP_FRAME_BACKUP_FILE, validateSleepFrameFile, &expected) ==
          StagedFileTransaction::Status::IoError ||
      !validateSleepFrameFile(SLEEP_FRAME_FILE, &expected)) {
    discard();
    return false;
  }
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  if (bufferSize != expected.payloadSize || !file.seek(SLEEP_FRAME_HEADER_SIZE)) {
    file.close();
    discard();
    return false;
  }

  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  const bool closed = file.close();
  if (bytesRead != bufferSize || !closed) {
    discard();
    return false;
  }
  if (consume) Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

}  // namespace SleepFrameStore
