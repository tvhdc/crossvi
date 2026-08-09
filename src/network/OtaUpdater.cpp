#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
// clang-format on

#include <SemanticVersion.h>
#include <Version.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "FirmwareFlasher.h"
#include "HttpTransportPolicy.h"

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/tvhdc/crossvi/releases/latest";
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaDigest.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the verified-https GET, redirects, and
  // User-Agent (see HttpDownloader).
  ReleaseJsonParser releaseParser;
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }
  if (!releaseParser.finish()) {
    LOG_ERR("OTA", "Release JSON was incomplete or malformed");
    return JSON_PARSE_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s digest=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no", releaseParser.foundFirmwareDigest() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }
  if (!releaseParser.foundFirmwareDigest()) {
    LOG_ERR("OTA", "No valid SHA-256 digest for firmware.bin");
    return JSON_PARSE_ERROR;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaDigest = releaseParser.getFirmwareDigest();
  otaSize = releaseParser.getFirmwareSize();
  if (!ota_version::isValid(latestVersion) || !ota_version::isValid(CROSSPOINT_VERSION)) {
    LOG_ERR("OTA", "Rejected invalid release version: %s", latestVersion.c_str());
    return JSON_PARSE_ERROR;
  }
  if (otaSize == 0) {
    LOG_ERR("OTA", "Rejected empty firmware asset");
    return JSON_PARSE_ERROR;
  }
  if (!HttpTransportPolicy::isHttpsUrl(otaUrl)) {
    LOG_ERR("OTA", "Rejected non-HTTPS firmware URL");
    updateAvailable = false;
    return JSON_PARSE_ERROR;
  }
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }
  return ota_version::isNewer(latestVersion, CROSSPOINT_VERSION);
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer() || !HttpTransportPolicy::isHttpsUrl(otaUrl) || otaDigest.size() != 64) {
    return UPDATE_OLDER_ERROR;
  }

  // Drive the OTA partition ourselves and stream the firmware through
  // HttpDownloader, reusing its verified TLS and redirect policy for the
  // GitHub -> CDN hop.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available");
    return INTERNAL_UPDATE_ERROR;
  }
  if (otaSize == 0 || otaSize > updatePartition->size) {
    LOG_ERR("OTA", "Firmware size %u exceeds OTA partition %u", static_cast<unsigned>(otaSize),
            static_cast<unsigned>(updatePartition->size));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  bool otaStarted = false;

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  bool wrongDevice = false;
  std::array<uint8_t, firmware_flash::IMAGE_CHIP_HEADER_SIZE> imageHeader{};
  size_t imageHeaderLength = 0;
  bool imageHeaderValidated = false;

  const auto writeChunk = [&](const uint8_t* data, const size_t len) {
    if (len == 0) return true;
    if (!otaStarted) {
      const esp_err_t beginResult = esp_ota_begin(updatePartition, otaSize, &otaHandle);
      if (beginResult != ESP_OK) {
        LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(beginResult));
        flashOk = false;
        return false;
      }
      otaStarted = true;
    }
    if (processedSize > otaSize || len > otaSize - processedSize) {
      LOG_ERR("OTA", "Firmware response exceeds declared size");
      flashOk = false;
      return false;
    }
    if (esp_ota_write(otaHandle, data, len) != ESP_OK) {
      flashOk = false;
      return false;
    }
    processedSize += len;
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    return true;
  };

  const bool fetchOk = HttpDownloader::fetchGithubReleaseAsset(otaUrl, otaDigest, [&](const uint8_t* data, size_t len) {
    if (!imageHeaderValidated) {
      const size_t needed = imageHeader.size() - imageHeaderLength;
      const size_t take = std::min(len, needed);
      std::memcpy(imageHeader.data() + imageHeaderLength, data, take);
      imageHeaderLength += take;
      data += take;
      len -= take;
      if (imageHeaderLength < imageHeader.size()) return true;

      uint16_t imageChipId = firmware_flash::UNKNOWN_CHIP_ID;
      firmware_flash::readImageChipId(imageHeader.data(), imageHeader.size(), imageChipId);
      const uint16_t deviceChipId = firmware_flash::runningPartitionChipId();
      if (!firmware_flash::imageChipMatchesDevice(imageChipId, deviceChipId)) {
        LOG_ERR("OTA", "wrong chip: image=0x%04X device=0x%04X", imageChipId, deviceChipId);
        wrongDevice = true;
        return false;
      }
      imageHeaderValidated = true;
      // TLS is already established. Only now allocate OTA state and write the
      // buffered header, so a wrong-device image never reaches flash.
      if (!writeChunk(imageHeader.data(), imageHeader.size())) return false;
    }
    return writeChunk(data, len);
  });

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (wrongDevice) {
    if (otaStarted) esp_ota_abort(otaHandle);
    return WRONG_DEVICE_ERROR;
  }

  if (!fetchOk || !flashOk || !imageHeaderValidated || !otaStarted || processedSize != otaSize) {
    if (fetchOk && processedSize != otaSize) {
      LOG_ERR("OTA", "Firmware size mismatch: received %u, declared %u", static_cast<unsigned>(processedSize),
              static_cast<unsigned>(otaSize));
    }
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    if (otaStarted) esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }

  esp_err_t esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
