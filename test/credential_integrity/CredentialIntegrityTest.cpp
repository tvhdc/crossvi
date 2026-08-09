#include <gtest/gtest.h>

#include <string>

#include "lib/Serialization/CredentialIntegrity.h"

TEST(CredentialIntegrity, DetectsSameLengthCorruption) {
  const std::string password = "password";
  const uint32_t checksum = credential_integrity::crc32(password);
  EXPECT_TRUE(credential_integrity::validate(password, password.size(), checksum));

  std::string corrupted = password;
  corrupted[3] ^= 1;
  EXPECT_FALSE(credential_integrity::validate(corrupted, password.size(), checksum));
  EXPECT_FALSE(credential_integrity::validate(password, password.size() + 1, checksum));
}

TEST(CredentialIntegrity, HandlesEmptyAndBinaryPasswords) {
  EXPECT_EQ(credential_integrity::crc32(""), 0U);
  const std::string binary{"a\0b", 3};
  const uint32_t checksum = credential_integrity::crc32(binary);
  EXPECT_TRUE(credential_integrity::validate(binary, 3, checksum));
}
