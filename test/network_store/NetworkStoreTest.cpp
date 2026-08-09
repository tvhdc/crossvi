#include <ArduinoJson.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>

#include "OpdsServerStore.h"
#include "WifiCredentialStore.h"
#include "lib/Serialization/CredentialIntegrity.h"

TEST(NetworkStore, OpdsRejectsOversizedFieldsAndRollsBackFailedWrites) {
  Storage.reset();
  auto& store = OPDS_STORE;
  ASSERT_TRUE(store.ensureLoaded());

  const OpdsServer original{"Original", "https://example.test/opds", "user", "secret"};
  ASSERT_TRUE(store.addServer(original));
  ASSERT_EQ(store.getCount(), 1U);

  Storage.makeUnwritable("/.crosspoint/opds.json.tmp");
  EXPECT_FALSE(store.updateServer(0, {"Changed", "https://changed.test", "other", "new"}));
  ASSERT_NE(store.getServer(0), nullptr);
  EXPECT_EQ(store.getServer(0)->name, "Original");
  EXPECT_FALSE(store.removeServer(0));
  EXPECT_EQ(store.getCount(), 1U);
  EXPECT_FALSE(store.addServer({"Second", "https://second.test", "", ""}));
  EXPECT_EQ(store.getCount(), 1U);
  Storage.makeWritable("/.crosspoint/opds.json.tmp");

  EXPECT_FALSE(store.addServer({std::string(64, 'n'), "", "", ""}));
  EXPECT_FALSE(store.addServer({"", std::string(128, 'u'), "", ""}));
  EXPECT_FALSE(store.addServer({"", "", std::string(64, 'u'), ""}));
  EXPECT_FALSE(store.addServer({"", "", "", std::string(64, 'p')}));
  EXPECT_EQ(store.getCount(), 1U);

  JsonDocument persisted;
  JsonArray servers = persisted["servers"].to<JsonArray>();
  JsonObject oversized = servers.add<JsonObject>();
  oversized["name"] = std::string(64, 'n');
  oversized["url"] = "https://invalid.test";
  JsonObject valid = servers.add<JsonObject>();
  valid["name"] = "Valid";
  valid["url"] = "https://valid.test";
  valid["password_obf"] = "secret";
  ASSERT_TRUE(store.fromJson(persisted.as<JsonVariantConst>()));
  ASSERT_EQ(store.getCount(), 1U);
  EXPECT_EQ(store.getServer(0)->name, "Valid");
}

TEST(NetworkStore, WifiRejectsInvalidFieldsAndRollsBackFailedWrites) {
  Storage.reset();
  auto& store = WIFI_STORE;
  EXPECT_FALSE(store.loadFromFile());

  ASSERT_TRUE(store.addCredential("Original", "password"));
  ASSERT_EQ(store.getCredentialCount(), 1U);

  Storage.makeUnwritable("/.crosspoint/wifi.json.tmp");
  EXPECT_FALSE(store.updateCredential(0, "Changed", "new-password"));
  ASSERT_TRUE(store.getCredentialAt(0));
  EXPECT_EQ(store.getCredentialAt(0)->ssid, "Original");
  EXPECT_FALSE(store.removeCredential("Original"));
  EXPECT_TRUE(store.hasSavedCredential("Original"));
  EXPECT_FALSE(store.addCredential("Second", "password"));
  EXPECT_EQ(store.getCredentialCount(), 1U);
  Storage.makeWritable("/.crosspoint/wifi.json.tmp");

  EXPECT_FALSE(store.addCredential("", ""));
  EXPECT_FALSE(store.addCredential(std::string(33, 's'), ""));
  EXPECT_FALSE(store.addCredential("Valid", std::string(65, 'p')));
  EXPECT_EQ(store.getCredentialCount(), 1U);

  ASSERT_TRUE(store.updateCredential(0, "Renamed", "new-password"));
  ASSERT_TRUE(store.getCredentialAt(0));
  EXPECT_EQ(store.getCredentialAt(0)->ssid, "Renamed");
  EXPECT_EQ(store.getCredentialAt(0)->password, "new-password");

  JsonDocument persisted;
  persisted["lastConnectedSsid"] = std::string(33, 's');
  JsonArray credentials = persisted["credentials"].to<JsonArray>();
  JsonObject oversized = credentials.add<JsonObject>();
  oversized["ssid"] = std::string(33, 's');
  oversized["password_obf"] = "password";
  JsonObject valid = credentials.add<JsonObject>();
  valid["ssid"] = "Valid";
  valid["password_obf"] = "password";
  valid["password_len"] = 8;
  valid["password_crc32"] = credential_integrity::crc32("password");
  ASSERT_TRUE(store.fromJson(persisted.as<JsonVariantConst>()));
  ASSERT_EQ(store.getCredentialCount(), 1U);
  EXPECT_EQ(store.getSsidAt(0), "Valid");
  EXPECT_TRUE(store.getLastConnectedSsid().empty());
}
