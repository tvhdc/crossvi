#include <gtest/gtest.h>

#include "Version/SemanticVersion.h"

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
