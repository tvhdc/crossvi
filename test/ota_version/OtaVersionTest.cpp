#include <gtest/gtest.h>

#include "Version/SemanticVersion.h"
#include "network/FirmwareFlasher.h"

TEST(OtaVersion, AcceptsOptionalVPrefixAndBuildSuffix) {
  EXPECT_TRUE(ota_version::isNewer("v1.4.2", "1.4.1"));
  EXPECT_TRUE(ota_version::isNewer("1.4.2+release", "v1.4.1-rc+abc"));
  EXPECT_TRUE(ota_version::isValid("1.4.1-dev-detached-c08a8813"));
}

TEST(OtaVersion, StableReleaseSupersedesSameNumericPrerelease) {
  EXPECT_TRUE(ota_version::isNewer("v1.4.2", "1.4.2-rc+abc"));
  EXPECT_FALSE(ota_version::isNewer("v1.4.2-rc", "1.4.2"));
  EXPECT_FALSE(ota_version::isNewer("v1.4.2", "1.4.2"));
}

TEST(OtaVersion, OrdersPrereleaseIdentifiersAccordingToSemver) {
  EXPECT_TRUE(ota_version::isNewer("1.4.2-rc.2", "1.4.2-rc.1"));
  EXPECT_TRUE(ota_version::isNewer("1.4.2-rc.10", "1.4.2-rc.2"));
  EXPECT_TRUE(ota_version::isNewer("1.4.2-beta", "1.4.2-alpha.9"));
  EXPECT_TRUE(ota_version::isNewer("1.4.2-alpha.1", "1.4.2-alpha"));
  EXPECT_FALSE(ota_version::isNewer("1.4.2-1", "1.4.2-alpha"));
  EXPECT_FALSE(ota_version::isNewer("1.4.2-rc.1+new", "1.4.2-rc.1+old"));
}

TEST(OtaVersion, RejectsMalformedOrOverflowingVersions) {
  EXPECT_FALSE(ota_version::isValid("v1.4"));
  EXPECT_FALSE(ota_version::isValid("1.4.2.extra"));
  EXPECT_FALSE(ota_version::isValid("1.4.2-"));
  EXPECT_FALSE(ota_version::isValid("1.4.2-rc..1"));
  EXPECT_FALSE(ota_version::isValid("1.4.2-rc.01"));
  EXPECT_FALSE(ota_version::isValid("1.4.2-rc_1"));
  EXPECT_FALSE(ota_version::isValid("1.4.2+build..1"));
  EXPECT_FALSE(ota_version::isValid("01.4.2"));
  EXPECT_FALSE(ota_version::isValid("1.4294967296.2"));
  EXPECT_FALSE(ota_version::isNewer("not-a-version", "1.4.1"));
}

TEST(FirmwareImageGuard, ReadsLittleEndianChipIdAndRejectsOtherDevices) {
  uint8_t header[firmware_flash::IMAGE_CHIP_HEADER_SIZE]{};
  header[firmware_flash::IMAGE_CHIP_ID_OFFSET] = 0x34;
  header[firmware_flash::IMAGE_CHIP_ID_OFFSET + 1] = 0x12;
  uint16_t chipId = 0;
  EXPECT_FALSE(firmware_flash::readImageChipId(header, firmware_flash::IMAGE_CHIP_HEADER_SIZE - 1, chipId));
  ASSERT_TRUE(firmware_flash::readImageChipId(header, sizeof(header), chipId));
  EXPECT_EQ(chipId, 0x1234);
  EXPECT_TRUE(firmware_flash::imageChipMatchesDevice(chipId, 0x1234));
  EXPECT_FALSE(firmware_flash::imageChipMatchesDevice(chipId, 0x5678));
  EXPECT_TRUE(firmware_flash::imageChipMatchesDevice(chipId, firmware_flash::UNKNOWN_CHIP_ID));
}
