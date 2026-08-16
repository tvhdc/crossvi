#pragma once

#include <HalStorage.h>
#include <ZipFile.h>

#include <cstddef>
#include <cstdint>
#include <string>

class RawSourceIdentityAccumulator {
 public:
  void update(const uint8_t* data, const size_t length) {
    for (size_t index = 0; index < length; ++index) {
      crc32_ ^= data[index];
      crc32_ = (crc32_ >> 4U) ^ CRC32_NIBBLE[crc32_ & 0x0FU];
      crc32_ = (crc32_ >> 4U) ^ CRC32_NIBBLE[crc32_ & 0x0FU];
      fnv64_ ^= data[index];
      fnv64_ *= FNV64_PRIME;
    }
  }

  [[nodiscard]] ZipFile::SourceIdentity finish(const uint64_t fileSize) const {
    return ZipFile::SourceIdentity::forRawFile(fileSize, ~crc32_, fnv64_);
  }

 private:
  static constexpr uint64_t FNV64_PRIME = 1099511628211ULL;
  static constexpr uint32_t CRC32_NIBBLE[16] = {
      0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU, 0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
      0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU, 0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU,
  };

  uint32_t crc32_ = UINT32_MAX;
  uint64_t fnv64_ = 14695981039346656037ULL;
};

// A complete source identity may be handed directly from a launcher/library
// preparation job to the reader only while the same FAT directory entry is
// still present. The legacy type name is retained for source compatibility;
// identities can describe either raw files or ZIP central directories.
struct RawSourceIdentityHandoff {
  std::string path;
  ZipFile::SourceIdentity identity{};
  uint16_t modifyDate = 0;
  uint16_t modifyTime = 0;
  bool valid = false;

  bool capture(const std::string& sourcePath, HalFile& file, const ZipFile::SourceIdentity& sourceIdentity) {
    uint16_t currentDate = 0;
    uint16_t currentTime = 0;
    if (!file.isOpen() || file.fileSize64() != sourceIdentity.fileSize ||
        !file.getModifyDateTime(&currentDate, &currentTime)) {
      return false;
    }
    return captureVerified(sourcePath, sourceIdentity, currentDate, currentTime);
  }

  bool captureVerified(const std::string& sourcePath, const ZipFile::SourceIdentity& sourceIdentity,
                       const uint16_t currentDate, const uint16_t currentTime) {
    if (sourcePath.empty()) return false;
    path = sourcePath;
    identity = sourceIdentity;
    modifyDate = currentDate;
    modifyTime = currentTime;
    valid = true;
    return true;
  }

  [[nodiscard]] bool matchesOpenFile(const std::string& sourcePath, HalFile& file) const {
    uint16_t currentDate = 0;
    uint16_t currentTime = 0;
    return valid && sourcePath == path && file.isOpen() && file.fileSize64() == identity.fileSize &&
           file.getModifyDateTime(&currentDate, &currentTime) && currentDate == modifyDate && currentTime == modifyTime;
  }
};
