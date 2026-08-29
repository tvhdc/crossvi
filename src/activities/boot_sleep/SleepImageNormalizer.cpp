#include "SleepImageNormalizer.h"

#include <Arduino.h>
#include <HalStorage.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string_view>

#include "Bitmap.h"
#include "PngImageSafety.h"
#include "PngToBmpConverter.h"
#include "SleepImageValidation.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_task_wdt.h>
#endif

namespace SleepImageNormalizer {
namespace {

constexpr size_t COPY_BUFFER_BYTES = 4096;
constexpr size_t COOPERATE_BYTES = 64U * 1024U;
constexpr int COOPERATE_ROWS = 16;
constexpr uint64_t COMPACT_BMP_OVERHEAD_ALLOWANCE = 4096;

using Target = SleepImageSelectionStore::Target;
using Validator = bool (*)(const char*);

void cooperate();

class CheckedOutput final : public Print {
 public:
  explicit CheckedOutput(HalFile& file, const bool cooperative = false) : file_(file), cooperative_(cooperative) {}
  using Print::write;

  size_t write(const uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t* data, const size_t length) override {
    const size_t written = file_.write(data, length);
    if (written != length) failed_ = true;
    if (cooperative_ && written == length && ++writesSinceCooperate_ >= COOPERATE_ROWS) {
      cooperate();
      writesSinceCooperate_ = 0;
    }
    return written;
  }

  bool failed() const { return failed_; }

 private:
  HalFile& file_;
  bool cooperative_ = false;
  bool failed_ = false;
  int writesSinceCooperate_ = 0;
};

void cooperate() {
#if defined(ARDUINO_ARCH_ESP32)
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
#endif
  yield();
}

bool removeIfPresent(const char* path) { return !Storage.exists(path) || Storage.remove(path); }

bool cleanStagingFiles() {
  bool clean = true;
  clean = removeIfPresent(stagingPath(Target::NormalBmp)) && clean;
  clean = removeIfPresent(stagingPath(Target::OverlayBmp)) && clean;
  clean = removeIfPresent(stagingPath(Target::OverlayPng)) && clean;
  return clean;
}

bool hasExtension(const std::string_view path, const std::string_view extension) {
  if (path.size() < extension.size()) return false;
  const size_t offset = path.size() - extension.size();
  for (size_t index = 0; index < extension.size(); ++index) {
    const auto lhs = static_cast<unsigned char>(path[offset + index]);
    const auto rhs = static_cast<unsigned char>(extension[index]);
    if (std::tolower(lhs) != std::tolower(rhs)) return false;
  }
  return true;
}

bool probeFileSize(const char* path, uint64_t& size) {
  HalFile file;
  if (!path || !Storage.openFileForRead("SLP", path, file)) return false;
  size = file.fileSize64();
  const bool clean = file.getError() == 0;
  return file.close() && clean;
}

uint32_t readBe32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) << 24U | static_cast<uint32_t>(bytes[1]) << 16U |
         static_cast<uint32_t>(bytes[2]) << 8U | static_cast<uint32_t>(bytes[3]);
}

void writeLe16(uint8_t* bytes, const uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
}

void writeLe32(uint8_t* bytes, const uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
  bytes[2] = static_cast<uint8_t>(value >> 16U);
  bytes[3] = static_cast<uint8_t>(value >> 24U);
}

void writeLe32Signed(uint8_t* bytes, const int32_t value) { writeLe32(bytes, static_cast<uint32_t>(value)); }

bool fitDimensions(const int sourceWidth, const int sourceHeight, const int screenWidth, const int screenHeight,
                   int& outputWidth, int& outputHeight) {
  if (sourceWidth <= 0 || sourceHeight <= 0 || screenWidth <= 0 || screenHeight <= 0) return false;
  if (sourceWidth <= screenWidth && sourceHeight <= screenHeight) {
    outputWidth = sourceWidth;
    outputHeight = sourceHeight;
    return true;
  }
  const uint64_t widthProduct = static_cast<uint64_t>(screenWidth) * static_cast<uint64_t>(sourceHeight);
  const uint64_t heightProduct = static_cast<uint64_t>(screenHeight) * static_cast<uint64_t>(sourceWidth);
  if (widthProduct <= heightProduct) {
    outputWidth = screenWidth;
    outputHeight = std::max(1, static_cast<int>(widthProduct / static_cast<uint64_t>(sourceWidth)));
  } else {
    outputHeight = screenHeight;
    outputWidth = std::max(
        1, static_cast<int>(static_cast<uint64_t>(sourceWidth) * screenHeight / static_cast<uint64_t>(sourceHeight)));
  }
  return outputWidth <= screenWidth && outputHeight <= screenHeight;
}

bool writeTwoBitHeader(Print& output, const int width, const int height, const bool topDown) {
  const uint64_t rowBytes = (static_cast<uint64_t>(width) * 2U + 31U) / 32U * 4U;
  const uint64_t imageBytes = rowBytes * static_cast<uint64_t>(height);
  const uint64_t fileBytes = 70U + imageBytes;
  if (fileBytes > std::numeric_limits<uint32_t>::max()) return false;

  std::array<uint8_t, 70> header{};
  header[0] = 'B';
  header[1] = 'M';
  writeLe32(header.data() + 2, static_cast<uint32_t>(fileBytes));
  writeLe32(header.data() + 10, 70);
  writeLe32(header.data() + 14, 40);
  writeLe32Signed(header.data() + 18, width);
  writeLe32Signed(header.data() + 22, topDown ? -height : height);
  writeLe16(header.data() + 26, 1);
  writeLe16(header.data() + 28, 2);
  writeLe32(header.data() + 34, static_cast<uint32_t>(imageBytes));
  writeLe32(header.data() + 38, 2835);
  writeLe32(header.data() + 42, 2835);
  writeLe32(header.data() + 46, 4);
  writeLe32(header.data() + 50, 4);
  constexpr std::array<uint8_t, 16> palette = {0,    0,    0,    0, 0x55, 0x55, 0x55, 0,
                                               0xAA, 0xAA, 0xAA, 0, 0xFF, 0xFF, 0xFF, 0};
  std::copy(palette.begin(), palette.end(), header.begin() + 54);
  return output.write(header.data(), header.size()) == header.size();
}

bool writeBgraHeader(Print& output, const int width, const int height, const bool topDown) {
  const uint64_t imageBytes = static_cast<uint64_t>(width) * 4U * static_cast<uint64_t>(height);
  const uint64_t fileBytes = 70U + imageBytes;
  if (fileBytes > std::numeric_limits<uint32_t>::max()) return false;

  std::array<uint8_t, 70> header{};
  header[0] = 'B';
  header[1] = 'M';
  writeLe32(header.data() + 2, static_cast<uint32_t>(fileBytes));
  writeLe32(header.data() + 10, 70);
  writeLe32(header.data() + 14, 40);
  writeLe32Signed(header.data() + 18, width);
  writeLe32Signed(header.data() + 22, topDown ? -height : height);
  writeLe16(header.data() + 26, 1);
  writeLe16(header.data() + 28, 32);
  writeLe32(header.data() + 30, 3);
  writeLe32(header.data() + 34, static_cast<uint32_t>(imageBytes));
  writeLe32(header.data() + 38, 2835);
  writeLe32(header.data() + 42, 2835);
  writeLe32(header.data() + 54, 0x00FF0000UL);
  writeLe32(header.data() + 58, 0x0000FF00UL);
  writeLe32(header.data() + 62, 0x000000FFUL);
  writeLe32(header.data() + 66, 0xFF000000UL);
  return output.write(header.data(), header.size()) == header.size();
}

Status finishWrittenFile(HalFile& input, HalFile& output, const bool converted, const bool outputWriteFailed,
                         const char* tempPath, const Validator validator, uint64_t& outputBytes) {
  const bool streamError = outputWriteFailed || input.getError() != 0 || output.getError() != 0;
  output.flush();
  const bool synced = !converted || streamError || output.sync();
  const bool outputClosed = output.close();
  const bool inputClosed = input.close();
  if (!converted) {
    const bool removed = removeIfPresent(tempPath);
    return streamError || !outputClosed || !inputClosed || !removed ? Status::IoError : Status::Invalid;
  }
  if (streamError || !synced || !outputClosed || !inputClosed) {
    removeIfPresent(tempPath);
    return Status::IoError;
  }
  if (!validator(tempPath)) {
    return removeIfPresent(tempPath) ? Status::Invalid : Status::IoError;
  }
  if (!probeFileSize(tempPath, outputBytes)) {
    removeIfPresent(tempPath);
    return Status::IoError;
  }
  return Status::Ready;
}

Status copyToStaging(const std::string& sourcePath, const uint64_t expectedBytes, const char* tempPath,
                     const Validator validator, uint64_t& outputBytes) {
  HalFile input;
  HalFile output;
  if (!Storage.openFileForRead("SLP", sourcePath, input)) return Status::IoError;
  if (input.fileSize64() != expectedBytes || !Storage.openFileForWrite("SLP", tempPath, output)) {
    input.close();
    return Status::IoError;
  }

  std::array<uint8_t, COPY_BUFFER_BYTES> buffer{};
  CheckedOutput checkedOutput(output);
  uint64_t copied = 0;
  size_t bytesSinceCooperate = 0;
  bool ok = true;
  while (copied < expectedBytes) {
    const size_t wanted =
        static_cast<size_t>(std::min<uint64_t>(buffer.size(), static_cast<uint64_t>(expectedBytes - copied)));
    const int read = input.read(buffer.data(), wanted);
    if (read != static_cast<int>(wanted) || checkedOutput.write(buffer.data(), wanted) != wanted) {
      ok = false;
      break;
    }
    copied += wanted;
    bytesSinceCooperate += wanted;
    if (bytesSinceCooperate >= COOPERATE_BYTES) {
      cooperate();
      bytesSinceCooperate = 0;
    }
  }
  return finishWrittenFile(input, output, ok && copied == expectedBytes, checkedOutput.failed(), tempPath, validator,
                           outputBytes);
}

Status convertPng(const std::string& sourcePath, const char* tempPath, const int width, const int height,
                  const bool alpha, uint64_t& outputBytes) {
  HalFile input;
  HalFile output;
  if (!Storage.openFileForRead("SLP", sourcePath, input)) return Status::IoError;
  if (!Storage.openFileForWrite("SLP", tempPath, output)) {
    input.close();
    return Status::IoError;
  }
  CheckedOutput checkedOutput(output, !alpha);
  const bool converted =
      alpha ? PngToBmpConverter::pngFileToBgraBmpStreamWithSize(input, checkedOutput, width, height)
            : PngToBmpConverter::pngFileToBmpStreamWithSize(input, checkedOutput, width, height, false);
  const Validator validator = alpha ? static_cast<Validator>(SleepImageValidation::overlayBmp)
                                    : static_cast<Validator>(SleepImageValidation::normalBmp);
  return finishWrittenFile(input, output, converted, checkedOutput.failed(), tempPath, validator, outputBytes);
}

class CountingPrint final : public Print {
 public:
  using Print::write;
  size_t write(const uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t*, const size_t length) override {
    if (length > std::numeric_limits<uint64_t>::max() - bytes_) return 0;
    bytes_ += length;
    return length;
  }

 private:
  uint64_t bytes_ = 0;
};

Status fullyDecodeOverlayPng(const std::string& sourcePath, const int width, const int height) {
  HalFile input;
  if (!Storage.openFileForRead("SLP", sourcePath, input)) return Status::IoError;
  CountingPrint discard;
  const bool decoded = PngToBmpConverter::pngFileToBgraBmpStreamWithSize(input, discard, width, height);
  const bool ioError = input.getError() != 0;
  const bool closed = input.close();
  if (ioError || !closed) return Status::IoError;
  return decoded ? Status::Ready : Status::Invalid;
}

struct PngInfo {
  int width = 0;
  int height = 0;
};

Status inspectPng(const std::string& sourcePath, PngInfo& info) {
  HalFile input;
  if (!Storage.openFileForRead("SLP", sourcePath, input)) return Status::IoError;
  std::array<uint8_t, 29> header{};
  const bool read = input.read(header.data(), header.size()) == static_cast<int>(header.size());
  const bool ioError = input.getError() != 0;
  const bool closed = input.close();
  if (ioError || !closed) return Status::IoError;
  constexpr std::array<uint8_t, 8> signature = {137, 80, 78, 71, 13, 10, 26, 10};
  if (!read || !std::equal(signature.begin(), signature.end(), header.begin()) || readBe32(header.data() + 8) != 13 ||
      std::memcmp(header.data() + 12, "IHDR", 4) != 0) {
    return Status::Invalid;
  }
  const uint32_t width = readBe32(header.data() + 16);
  const uint32_t height = readBe32(header.data() + 20);
  if (!png_image_safety::validIhdr(13, width, height, header[24], header[25], header[26], header[27], header[28])) {
    return Status::Invalid;
  }
  info.width = static_cast<int>(width);
  info.height = static_cast<int>(height);
  return Status::Ready;
}

struct BmpInfo {
  int width = 0;
  int height = 0;
  int rowBytes = 0;
  uint16_t bpp = 0;
};

Status inspectBitmap(HalFile& input, BmpInfo& info) {
  Bitmap bitmap(input);
  const BmpReaderError parsed = bitmap.parseHeaders();
  info = {bitmap.getWidth(), bitmap.getHeight(), bitmap.getRowBytes(), bitmap.getBpp()};
  const bool ioError = input.getError() != 0;
  const bool closed = input.close();
  if (ioError || !closed) return Status::IoError;
  return parsed == BmpReaderError::Ok ? Status::Ready : Status::Invalid;
}

Status inspectNormalBmp(const std::string& sourcePath, BmpInfo& info) {
  HalFile input;
  if (!Storage.openFileForRead("SLP", sourcePath, input)) return Status::IoError;
  return inspectBitmap(input, info);
}

Status inspectOverlayBmp(const std::string& sourcePath, BmpInfo& info) {
  HalFile input;
  if (!Storage.openFileForRead("SLP", sourcePath, input)) return Status::IoError;
  SleepImageValidation::Bmp32Header alphaHeader;
  const SleepImageValidation::Bmp32HeaderStatus alphaStatus = SleepImageValidation::readBmp32Header(input, alphaHeader);
  if (alphaStatus == SleepImageValidation::Bmp32HeaderStatus::Valid) {
    info = {alphaHeader.width, alphaHeader.height, static_cast<int>(alphaHeader.rowBytes), 32};
    const bool ioError = input.getError() != 0;
    const bool closed = input.close();
    return ioError || !closed ? Status::IoError : Status::Ready;
  }
  if (alphaStatus == SleepImageValidation::Bmp32HeaderStatus::Invalid || !input.seek(0)) {
    const bool ioError = input.getError() != 0;
    const bool closed = input.close();
    return ioError || !closed ? Status::IoError : Status::Invalid;
  }
  return inspectBitmap(input, info);
}

Status convertBmpToTwoBit(const std::string& sourcePath, const char* tempPath, const int screenWidth,
                          const int screenHeight, uint64_t& outputBytes) {
  HalFile input;
  HalFile output;
  if (!Storage.openFileForRead("SLP", sourcePath, input)) return Status::IoError;
  Bitmap bitmap(input, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    const bool ioError = input.getError() != 0;
    const bool closed = input.close();
    return ioError || !closed ? Status::IoError : Status::Invalid;
  }

  int outputWidth = 0;
  int outputHeight = 0;
  if (!fitDimensions(bitmap.getWidth(), bitmap.getHeight(), screenWidth, screenHeight, outputWidth, outputHeight) ||
      !Storage.openFileForWrite("SLP", tempPath, output)) {
    input.close();
    return Status::IoError;
  }

  const int sourcePackedBytes = (bitmap.getWidth() + 3) / 4;
  const int outputRowBytes = (outputWidth * 2 + 31) / 32 * 4;
  auto sourcePacked = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[sourcePackedBytes]);
  auto sourceRow = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[bitmap.getRowBytes()]);
  auto outputRow = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[outputRowBytes]);
  const bool allocationFailed = !sourcePacked || !sourceRow || !outputRow;
  CheckedOutput checkedOutput(output);
  bool ok = !allocationFailed && writeTwoBitHeader(checkedOutput, outputWidth, outputHeight, bitmap.isTopDown());
  int currentSourceRow = -1;
  for (int outputY = 0; ok && outputY < outputHeight; ++outputY) {
    const int wantedSourceRow = static_cast<int>(static_cast<int64_t>(outputY) * bitmap.getHeight() / outputHeight);
    while (currentSourceRow < wantedSourceRow) {
      ok = bitmap.readNextRow(sourcePacked.get(), sourceRow.get()) == BmpReaderError::Ok;
      ++currentSourceRow;
      if (!ok) break;
    }
    if (!ok) break;
    std::memset(outputRow.get(), 0, outputRowBytes);
    for (int outputX = 0; outputX < outputWidth; ++outputX) {
      const int sourceX = static_cast<int>(static_cast<int64_t>(outputX) * bitmap.getWidth() / outputWidth);
      const uint8_t level = (sourcePacked[sourceX / 4] >> static_cast<unsigned>(6 - (sourceX % 4) * 2)) & 0x03U;
      outputRow[outputX / 4] |= level << static_cast<unsigned>(6 - (outputX % 4) * 2);
    }
    ok = checkedOutput.write(outputRow.get(), outputRowBytes) == static_cast<size_t>(outputRowBytes);
    if ((outputY + 1) % COOPERATE_ROWS == 0) cooperate();
  }
  while (ok && currentSourceRow + 1 < bitmap.getHeight()) {
    ok = bitmap.readNextRow(sourcePacked.get(), sourceRow.get()) == BmpReaderError::Ok;
    ++currentSourceRow;
    if ((currentSourceRow + 1) % COOPERATE_ROWS == 0) cooperate();
  }
  const Status finished = finishWrittenFile(input, output, ok, checkedOutput.failed(), tempPath,
                                            SleepImageValidation::normalBmp, outputBytes);
  return allocationFailed ? Status::IoError : finished;
}

Status convertBmp32ToBgra(HalFile& input, const SleepImageValidation::Bmp32Header& header, Print& output,
                          const int screenWidth, const int screenHeight) {
  int outputWidth = 0;
  int outputHeight = 0;
  if (!fitDimensions(header.width, header.height, screenWidth, screenHeight, outputWidth, outputHeight) ||
      !writeBgraHeader(output, outputWidth, outputHeight, header.topDown) || !input.seek64(header.pixelOffset)) {
    return Status::Invalid;
  }
  auto sourceRow = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[header.rowBytes]);
  auto outputRow = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[static_cast<size_t>(outputWidth) * 4U]);
  if (!sourceRow || !outputRow) return Status::IoError;

  bool ok = true;
  int currentSourceRow = -1;
  for (int outputY = 0; ok && outputY < outputHeight; ++outputY) {
    const int wantedSourceRow = static_cast<int>(static_cast<int64_t>(outputY) * header.height / outputHeight);
    while (currentSourceRow < wantedSourceRow) {
      ok = input.read(sourceRow.get(), header.rowBytes) == static_cast<int>(header.rowBytes);
      ++currentSourceRow;
      if (!ok) break;
    }
    if (!ok) break;
    for (int outputX = 0; outputX < outputWidth; ++outputX) {
      const int sourceX = static_cast<int>(static_cast<int64_t>(outputX) * header.width / outputWidth);
      std::memcpy(outputRow.get() + static_cast<size_t>(outputX) * 4U,
                  sourceRow.get() + static_cast<size_t>(sourceX) * 4U, 4);
    }
    ok = output.write(outputRow.get(), static_cast<size_t>(outputWidth) * 4U) == static_cast<size_t>(outputWidth) * 4U;
    if ((outputY + 1) % COOPERATE_ROWS == 0) cooperate();
  }
  while (ok && currentSourceRow + 1 < header.height) {
    ok = input.read(sourceRow.get(), header.rowBytes) == static_cast<int>(header.rowBytes);
    ++currentSourceRow;
    if ((currentSourceRow + 1) % COOPERATE_ROWS == 0) cooperate();
  }
  return ok ? Status::Ready : Status::Invalid;
}

Status convertWhiteKeyBmpToBgra(HalFile& input, Print& output, const int screenWidth, const int screenHeight) {
  if (!input.seek(0)) return Status::Invalid;
  Bitmap bitmap(input, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return Status::Invalid;
  int outputWidth = 0;
  int outputHeight = 0;
  if (!fitDimensions(bitmap.getWidth(), bitmap.getHeight(), screenWidth, screenHeight, outputWidth, outputHeight) ||
      !writeBgraHeader(output, outputWidth, outputHeight, bitmap.isTopDown())) {
    return Status::Invalid;
  }

  const int sourcePackedBytes = (bitmap.getWidth() + 3) / 4;
  auto sourcePacked = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[sourcePackedBytes]);
  auto sourceRow = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[bitmap.getRowBytes()]);
  auto outputRow = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[static_cast<size_t>(outputWidth) * 4U]);
  if (!sourcePacked || !sourceRow || !outputRow) return Status::IoError;

  bool ok = true;
  int currentSourceRow = -1;
  for (int outputY = 0; ok && outputY < outputHeight; ++outputY) {
    const int wantedSourceRow = static_cast<int>(static_cast<int64_t>(outputY) * bitmap.getHeight() / outputHeight);
    while (currentSourceRow < wantedSourceRow) {
      ok = bitmap.readNextRow(sourcePacked.get(), sourceRow.get()) == BmpReaderError::Ok;
      ++currentSourceRow;
      if (!ok) break;
    }
    if (!ok) break;
    for (int outputX = 0; outputX < outputWidth; ++outputX) {
      const int sourceX = static_cast<int>(static_cast<int64_t>(outputX) * bitmap.getWidth() / outputWidth);
      const uint8_t level = (sourcePacked[sourceX / 4] >> static_cast<unsigned>(6 - (sourceX % 4) * 2)) & 0x03U;
      uint8_t* pixel = outputRow.get() + static_cast<size_t>(outputX) * 4U;
      pixel[0] = pixel[1] = pixel[2] = static_cast<uint8_t>(level * 85U);
      pixel[3] = level >= 3 ? 0 : 255;
    }
    ok = output.write(outputRow.get(), static_cast<size_t>(outputWidth) * 4U) == static_cast<size_t>(outputWidth) * 4U;
    if ((outputY + 1) % COOPERATE_ROWS == 0) cooperate();
  }
  while (ok && currentSourceRow + 1 < bitmap.getHeight()) {
    ok = bitmap.readNextRow(sourcePacked.get(), sourceRow.get()) == BmpReaderError::Ok;
    ++currentSourceRow;
    if ((currentSourceRow + 1) % COOPERATE_ROWS == 0) cooperate();
  }
  return ok ? Status::Ready : Status::Invalid;
}

Status convertBmpToBgra(const std::string& sourcePath, const char* tempPath, const int screenWidth,
                        const int screenHeight, uint64_t& outputBytes) {
  HalFile input;
  HalFile output;
  if (!Storage.openFileForRead("SLP", sourcePath, input)) return Status::IoError;
  if (!Storage.openFileForWrite("SLP", tempPath, output)) {
    input.close();
    return Status::IoError;
  }

  SleepImageValidation::Bmp32Header alphaHeader;
  const auto alphaStatus = SleepImageValidation::readBmp32Header(input, alphaHeader);
  CheckedOutput checkedOutput(output);
  Status converted = Status::Invalid;
  if (alphaStatus == SleepImageValidation::Bmp32HeaderStatus::Valid) {
    converted = convertBmp32ToBgra(input, alphaHeader, checkedOutput, screenWidth, screenHeight);
  } else if (alphaStatus == SleepImageValidation::Bmp32HeaderStatus::Not32Bit) {
    converted = convertWhiteKeyBmpToBgra(input, checkedOutput, screenWidth, screenHeight);
  }
  const bool ioError = input.getError() != 0 || output.getError() != 0;
  const Status finished = finishWrittenFile(input, output, converted == Status::Ready, checkedOutput.failed(), tempPath,
                                            SleepImageValidation::overlayBmp, outputBytes);
  if (converted == Status::IoError || ioError) return Status::IoError;
  return finished;
}

bool compactNormalBmp(const BmpInfo& info, const uint64_t sourceBytes, const int screenWidth, const int screenHeight) {
  if (!(info.bpp == 1 || info.bpp == 2) || info.width > screenWidth || info.height > screenHeight) return false;
  const uint64_t compactLimit =
      COMPACT_BMP_OVERHEAD_ALLOWANCE + static_cast<uint64_t>(info.rowBytes) * static_cast<uint64_t>(info.height);
  return sourceBytes <= compactLimit;
}

Result failedResult(Result result, const Status status) {
  result.status = status;
  if (status != Status::TooLarge && !cleanStagingFiles()) result.status = Status::IoError;
  return result;
}

}  // namespace

const char* stagingPath(const Target target) {
  switch (target) {
    case Target::NormalBmp:
      return "/sleep.bmp.tmp";
    case Target::OverlayBmp:
      return "/sleep-overlay.bmp.tmp";
    case Target::OverlayPng:
      return "/sleep-overlay.png.tmp";
  }
  return "";
}

Result prepare(const std::string& sourcePath, const bool transparent, const int screenWidth, const int screenHeight) {
  Result result;
  if (sourcePath.empty() || screenWidth <= 0 || screenHeight <= 0) return result;
  if (!probeFileSize(sourcePath.c_str(), result.sourceBytes)) {
    result.status = Storage.exists(sourcePath.c_str()) ? Status::IoError : Status::Invalid;
    return result;
  }
  if (result.sourceBytes > MAX_SOURCE_BYTES) {
    result.status = Status::TooLarge;
    return result;
  }
  if (result.sourceBytes == 0) return result;
  if (!SleepImageSelectionStore::recover()) {
    result.status = Status::IoError;
    return result;
  }

  const bool png = hasExtension(sourcePath, ".png");
  const bool bmp = hasExtension(sourcePath, ".bmp");
  if (!png && !bmp) return failedResult(result, Status::Invalid);

  Status status = Status::Invalid;
  if (!transparent) {
    result.target = Target::NormalBmp;
    if (png) {
      result.optimized = true;
      status = convertPng(sourcePath, stagingPath(result.target), screenWidth, screenHeight, false, result.outputBytes);
    } else {
      BmpInfo info;
      status = inspectNormalBmp(sourcePath, info);
      if (status == Status::Ready && compactNormalBmp(info, result.sourceBytes, screenWidth, screenHeight)) {
        status = copyToStaging(sourcePath, result.sourceBytes, stagingPath(result.target),
                               SleepImageValidation::normalBmp, result.outputBytes);
      } else if (status == Status::Ready) {
        result.optimized = true;
        status =
            convertBmpToTwoBit(sourcePath, stagingPath(result.target), screenWidth, screenHeight, result.outputBytes);
      }
    }
  } else if (png) {
    PngInfo info;
    status = inspectPng(sourcePath, info);
    const bool inBounds = status == Status::Ready && info.width <= screenWidth && info.height <= screenHeight;
    const bool compact = result.sourceBytes <= MAX_PASSTHROUGH_OVERLAY_BYTES;
    const bool canRenderDirectly = inBounds && compact && SleepImageValidation::overlayPng(sourcePath);
    if (status == Status::Ready && canRenderDirectly) {
      result.target = Target::OverlayPng;
      status = copyToStaging(sourcePath, result.sourceBytes, stagingPath(result.target),
                             SleepImageValidation::overlayPng, result.outputBytes);
      if (status == Status::Ready) {
        status = fullyDecodeOverlayPng(stagingPath(result.target), screenWidth, screenHeight);
      }
    } else if (status == Status::Ready) {
      result.target = Target::OverlayBmp;
      result.optimized = true;
      status = convertPng(sourcePath, stagingPath(result.target), screenWidth, screenHeight, true, result.outputBytes);
    }
  } else {
    BmpInfo info;
    status = inspectOverlayBmp(sourcePath, info);
    const bool inBounds = status == Status::Ready && info.width <= screenWidth && info.height <= screenHeight;
    const bool compact = result.sourceBytes <= MAX_PASSTHROUGH_OVERLAY_BYTES;
    if (status == Status::Ready && inBounds && compact) {
      result.target = Target::OverlayBmp;
      status = copyToStaging(sourcePath, result.sourceBytes, stagingPath(result.target),
                             SleepImageValidation::overlayBmp, result.outputBytes);
    } else if (status == Status::Ready) {
      result.target = Target::OverlayBmp;
      result.optimized = true;
      status = convertBmpToBgra(sourcePath, stagingPath(result.target), screenWidth, screenHeight, result.outputBytes);
    }
  }

  if (status != Status::Ready) return failedResult(result, status);
  result.status = Status::Ready;
  return result;
}

}  // namespace SleepImageNormalizer
