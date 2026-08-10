#include <ArduinoJson.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>

#include "KOReaderCredentialStore.h"
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
  store.setLastConnectedSsid("Original");
  ASSERT_EQ(store.getLastConnectedSsid(), "Original");

  Storage.makeUnwritable("/.crosspoint/wifi.json.tmp");
  store.setLastConnectedSsid("Changed");
  EXPECT_EQ(store.getLastConnectedSsid(), "Original");
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

TEST(NetworkStore, InvalidKOReaderMatchMethodIsRepairedOnDisk) {
  Storage.reset();
  auto& store = KOREADER_STORE;
  ASSERT_TRUE(store.ensureLoaded());

  JsonDocument malformed;
  malformed["cfgVersion"] = 3;
  malformed["username"] = "reader";
  malformed["password_obf"] = "secret";
  malformed["serverUrl"] = "";
  malformed["matchMethod"] = 255;
  malformed["sendMetadata"] = false;
  malformed["syncBehavior"] = 0;
  ASSERT_TRUE(store.fromJson(malformed.as<JsonVariantConst>()));
  EXPECT_EQ(store.getMatchMethod(), DocumentMatchMethod::FILENAME);

  ASSERT_TRUE(Storage.exists(KOReaderCredentialStore::getFilePath()));
  const auto& bytes = Storage.file(KOReaderCredentialStore::getFilePath());
  JsonDocument repaired;
  ASSERT_FALSE(deserializeJson(repaired, bytes.data(), bytes.size()));
  EXPECT_EQ(repaired["matchMethod"].as<uint8_t>(), static_cast<uint8_t>(DocumentMatchMethod::FILENAME));
}

TEST(NetworkStore, FailedCredentialClearsLeaveMemoryUnchanged) {
  Storage.reset();

  auto& koReaderStore = KOREADER_STORE;
  ASSERT_TRUE(koReaderStore.ensureLoaded());
  koReaderStore.setCredentials("reader", "secret");
  ASSERT_TRUE(koReaderStore.saveToFile());

  Storage.makeUnwritable("/.crosspoint/koreader.json.tmp");
  koReaderStore.clearCredentials();
  EXPECT_EQ(koReaderStore.getUsername(), "reader");
  EXPECT_EQ(koReaderStore.getPassword(), "secret");
  Storage.makeWritable("/.crosspoint/koreader.json.tmp");

  auto& wifiStore = WIFI_STORE;
  ASSERT_TRUE(wifiStore.addCredential("Network", "password"));
  wifiStore.setLastConnectedSsid("Network");
  ASSERT_EQ(wifiStore.getLastConnectedSsid(), "Network");

  Storage.makeUnwritable("/.crosspoint/wifi.json.tmp");
  wifiStore.clearLastConnectedSsid();
  EXPECT_EQ(wifiStore.getLastConnectedSsid(), "Network");
  wifiStore.clearAll();
  EXPECT_TRUE(wifiStore.hasSavedCredential("Network"));
  EXPECT_EQ(wifiStore.getLastConnectedSsid(), "Network");
  Storage.makeWritable("/.crosspoint/wifi.json.tmp");

  koReaderStore.clearCredentials();
  EXPECT_TRUE(koReaderStore.getUsername().empty());
  EXPECT_TRUE(koReaderStore.getPassword().empty());
  const auto& koReaderBytes = Storage.file(KOReaderCredentialStore::getFilePath());
  JsonDocument persistedKOReader;
  ASSERT_FALSE(deserializeJson(persistedKOReader, koReaderBytes.data(), koReaderBytes.size()));
  EXPECT_STREQ(persistedKOReader["username"].as<const char*>(), "");

  wifiStore.clearAll();
  EXPECT_EQ(wifiStore.getCredentialCount(), 0U);
  EXPECT_TRUE(wifiStore.getLastConnectedSsid().empty());
  const auto& wifiBytes = Storage.file(WifiCredentialStore::getFilePath());
  JsonDocument persistedWifi;
  ASSERT_FALSE(deserializeJson(persistedWifi, wifiBytes.data(), wifiBytes.size()));
  EXPECT_EQ(persistedWifi["credentials"].as<JsonArrayConst>().size(), 0U);
  EXPECT_STREQ(persistedWifi["lastConnectedSsid"].as<const char*>(), "");
}
