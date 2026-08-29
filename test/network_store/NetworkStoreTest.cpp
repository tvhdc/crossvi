#include <ArduinoJson.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "WifiCredentialStore.h"
#include "lib/Serialization/CredentialIntegrity.h"

TEST(NetworkStore, OpdsRejectsOversizedFieldsAndRollsBackFailedWrites) {
  Storage.reset();
  auto& store = OPDS_STORE;
  ASSERT_TRUE(store.ensureLoaded());

  const OpdsServer original{"Original", "https://example.test/opds", "user", "secret"};
  ASSERT_TRUE(store.addServer(original));
  ASSERT_EQ(store.getCount(), 1U);

  Storage.resetIoCounters();
  ASSERT_TRUE(store.updateServer(0, original));
  EXPECT_EQ(Storage.openReadAttemptsFor(OpdsServerStore::getFilePath()), 0U);
  EXPECT_EQ(Storage.openWriteAttemptsFor("/.crosspoint/opds.json.tmp"), 0U);

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

  Storage.resetIoCounters();
  ASSERT_TRUE(store.updateCredential(0, "Original", "password"));
  EXPECT_EQ(Storage.openReadAttemptsFor(WifiCredentialStore::getFilePath()), 0U);
  EXPECT_EQ(Storage.openWriteAttemptsFor("/.crosspoint/wifi.json.tmp"), 0U);

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

  Storage.reset();
  JsonDocument empty;
  empty["lastConnectedSsid"] = "";
  empty["credentials"].to<JsonArray>();
  ASSERT_TRUE(store.fromJson(empty.as<JsonVariantConst>()));
  Storage.resetIoCounters();
  ASSERT_TRUE(store.addCredential("Connected", "password", true));
  EXPECT_EQ(store.getLastConnectedSsid(), "Connected");
  EXPECT_EQ(Storage.openWriteAttemptsFor("/.crosspoint/wifi.json.tmp"), 1U);

  JsonDocument connected;
  const auto& connectedBytes = Storage.file(WifiCredentialStore::getFilePath());
  ASSERT_FALSE(deserializeJson(connected, connectedBytes.data(), connectedBytes.size()));
  EXPECT_STREQ(connected["lastConnectedSsid"].as<const char*>(), "Connected");
  ASSERT_EQ(connected["credentials"].as<JsonArrayConst>().size(), 1U);
  EXPECT_STREQ(connected["credentials"][0]["ssid"].as<const char*>(), "Connected");
}

TEST(NetworkStore, InvalidKOReaderMatchMethodIsRepairedOnDisk) {
  Storage.reset();
  auto& store = KOREADER_STORE;
  ASSERT_TRUE(store.ensureLoaded());
  EXPECT_EQ(store.getBaseUrl(), "https://sync.crosspointreader.com");

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

TEST(NetworkStore, OversizedKOReaderCredentialsAreClearedBeforeUse) {
  Storage.reset();
  auto& store = KOREADER_STORE;
  ASSERT_TRUE(store.ensureLoaded());

  const auto loadCredentials = [&store](const std::string& username, const std::string& password) {
    JsonDocument doc;
    doc["cfgVersion"] = 3;
    doc["username"] = username;
    doc["password_obf"] = password;
    doc["serverUrl"] = KOReaderCredentialStore::crossPointServerUrl();
    doc["matchMethod"] = 0;
    doc["sendMetadata"] = false;
    doc["syncBehavior"] = 0;
    return store.fromJson(doc.as<JsonVariantConst>());
  };

  ASSERT_TRUE(loadCredentials(std::string(KOReaderCredentialStore::MAX_USERNAME_BYTES + 1, 'u'), "secret"));
  EXPECT_FALSE(store.hasCredentials());
  EXPECT_TRUE(store.getUsername().empty());
  EXPECT_TRUE(store.getPassword().empty());

  ASSERT_TRUE(loadCredentials("reader", std::string(KOReaderCredentialStore::MAX_PASSWORD_BYTES + 1, 'p')));
  EXPECT_FALSE(store.hasCredentials());
  EXPECT_TRUE(store.getUsername().empty());
  EXPECT_TRUE(store.getPassword().empty());
}

TEST(NetworkStore, InvalidKOReaderServerDoesNotApplyPartiallyParsedCredentials) {
  Storage.reset();
  auto& store = KOREADER_STORE;
  ASSERT_TRUE(store.ensureLoaded());

  JsonDocument valid;
  valid["cfgVersion"] = 3;
  valid["username"] = "existing";
  valid["password_obf"] = "existing-secret";
  valid["serverUrl"] = KOReaderCredentialStore::koSyncServerUrl();
  valid["matchMethod"] = 0;
  valid["sendMetadata"] = false;
  valid["syncBehavior"] = 0;
  ASSERT_TRUE(store.fromJson(valid.as<JsonVariantConst>()));

  JsonDocument invalid;
  invalid["cfgVersion"] = 3;
  invalid["username"] = "partially-parsed";
  invalid["password_obf"] = "wrong-destination-secret";
  invalid["serverUrl"] = std::string(KOReaderCredentialStore::MAX_SERVER_URL_BYTES + 1, 's');
  invalid["matchMethod"] = 0;
  invalid["sendMetadata"] = false;
  invalid["syncBehavior"] = 0;
  ASSERT_FALSE(store.fromJson(invalid.as<JsonVariantConst>()));

  EXPECT_EQ(store.getUsername(), "existing");
  EXPECT_EQ(store.getPassword(), "existing-secret");
  EXPECT_EQ(store.getBaseUrl(), KOReaderCredentialStore::koSyncServerUrl());
}

TEST(NetworkStore, KOReaderDocumentIdRejectsShortSdReads) {
  Storage.reset();
  Storage.setFile("/book.epub", std::vector<uint8_t>(2048, 0x5a));

  ASSERT_FALSE(KOReaderDocumentId::calculate("/book.epub").empty());

  Storage.shortReadFor("/book.epub");
  EXPECT_TRUE(KOReaderDocumentId::calculate("/book.epub").empty());
}

TEST(NetworkStore, MissingWifiStoreClearsCredentialsFromPreviousStorage) {
  Storage.reset();
  auto& store = WIFI_STORE;

  JsonDocument empty;
  empty["lastConnectedSsid"] = "";
  empty["credentials"].to<JsonArray>();
  ASSERT_TRUE(store.fromJson(empty.as<JsonVariantConst>()));
  ASSERT_TRUE(store.addCredential("Old card", "password", true));
  ASSERT_EQ(store.getCredentialCount(), 1U);
  ASSERT_EQ(store.getLastConnectedSsid(), "Old card");

  // Simulate replacing the SD card with one that has no Wi-Fi store. The
  // singleton remains alive across reloads, so stale credentials must not.
  Storage.reset();
  EXPECT_FALSE(store.loadFromFile());
  EXPECT_EQ(store.getCredentialCount(), 0U);
  EXPECT_TRUE(store.getLastConnectedSsid().empty());
}

TEST(NetworkStore, UnreadableWifiStoreClearsCredentialsFromPreviousLoad) {
  Storage.reset();
  auto& store = WIFI_STORE;

  ASSERT_TRUE(store.addCredential("Old card", "password", true));
  ASSERT_EQ(store.getCredentialCount(), 1U);
  ASSERT_EQ(store.getLastConnectedSsid(), "Old card");

  const std::string path = WifiCredentialStore::getFilePath();
  Storage.makeUnreadable(path);
  Storage.makeUnreadable(path + ".bak");
  Storage.makeUnreadable(path + ".tmp");

  EXPECT_FALSE(store.loadFromFile());
  EXPECT_EQ(store.getCredentialCount(), 0U);
  EXPECT_TRUE(store.getLastConnectedSsid().empty());

  // An I/O error intentionally leaves persistence read-only for this boot.
  // Reloading a missing store restores the normal first-boot state so this
  // singleton does not leak fault-injection state into later tests.
  Storage.reset();
  EXPECT_FALSE(store.loadFromFile());
}

TEST(NetworkStore, KOReaderServerListDefaultsUnconfiguredStoresAndPreservesLegacyActiveServer) {
  Storage.reset();
  auto& store = KOREADER_STORE;
  ASSERT_TRUE(store.ensureLoaded());

  JsonDocument unconfigured;
  unconfigured["cfgVersion"] = 3;
  unconfigured["username"] = "";
  unconfigured["password_obf"] = "";
  unconfigured["serverUrl"] = "";
  unconfigured["matchMethod"] = 0;
  unconfigured["sendMetadata"] = false;
  unconfigured["syncBehavior"] = 0;
  ASSERT_TRUE(store.fromJson(unconfigured.as<JsonVariantConst>()));
  EXPECT_FALSE(store.hasCredentials());
  EXPECT_EQ(store.getBaseUrl(), "https://sync.crosspointreader.com");
  JsonDocument unconfiguredSaved;
  const auto& unconfiguredBytes = Storage.file(KOReaderCredentialStore::getFilePath());
  ASSERT_FALSE(deserializeJson(unconfiguredSaved, unconfiguredBytes.data(), unconfiguredBytes.size()));
  EXPECT_STREQ(unconfiguredSaved["serverUrl"].as<const char*>(), "https://sync.crosspointreader.com");

  JsonDocument legacy;
  legacy["cfgVersion"] = 3;
  legacy["username"] = "reader";
  legacy["password_obf"] = "secret";
  legacy["serverUrl"] = "";
  legacy["matchMethod"] = 0;
  legacy["sendMetadata"] = false;
  legacy["syncBehavior"] = 0;
  ASSERT_TRUE(store.fromJson(legacy.as<JsonVariantConst>()));

  EXPECT_EQ(store.getBaseUrl(), "https://sync.koreader.rocks:443");
  ASSERT_EQ(store.getCustomServers().size(), 1U);
  EXPECT_EQ(store.getCustomServers().front(), "https://sync.koreader.rocks:443");

  JsonDocument current;
  current["cfgVersion"] = 3;
  current["username"] = "reader";
  current["password_obf"] = "secret";
  current["serverUrl"] = "https://sync.crosspointreader.com";
  current["matchMethod"] = 0;
  current["sendMetadata"] = false;
  current["syncBehavior"] = 0;
  current["customServers"].to<JsonArray>();
  ASSERT_TRUE(store.fromJson(current.as<JsonVariantConst>()));
  EXPECT_EQ(store.getBaseUrl(), "https://sync.crosspointreader.com");
  EXPECT_TRUE(store.getCustomServers().empty());

  store.setServerUrl("https://web.example");
  JsonDocument webSaved;
  store.toJson(webSaved);
  ASSERT_EQ(webSaved["customServers"].as<JsonArrayConst>().size(), 1U);
  EXPECT_STREQ(webSaved["customServers"][0].as<const char*>(), "https://web.example");
  ASSERT_EQ(store.getCustomServers().size(), 1U);
  EXPECT_EQ(store.getCustomServers().front(), "https://web.example");

  JsonDocument capped;
  capped["cfgVersion"] = 3;
  capped["username"] = "reader";
  capped["password_obf"] = "secret";
  capped["serverUrl"] = "https://selected.example";
  capped["matchMethod"] = 0;
  capped["sendMetadata"] = false;
  capped["syncBehavior"] = 0;
  JsonArray cappedServers = capped["customServers"].to<JsonArray>();
  for (size_t i = 0; i < KOReaderCredentialStore::MAX_CUSTOM_SERVERS; ++i) {
    cappedServers.add("https://saved" + std::to_string(i) + ".example");
  }
  ASSERT_TRUE(store.fromJson(capped.as<JsonVariantConst>()));
  ASSERT_EQ(store.getCustomServers().size(), KOReaderCredentialStore::MAX_CUSTOM_SERVERS);
  EXPECT_EQ(store.getCustomServers().back(), "https://selected.example");
}

TEST(NetworkStore, KOReaderCustomServerCrudIsBoundedAndRollsBackFailedWrites) {
  Storage.reset();
  auto& store = KOREADER_STORE;
  ASSERT_TRUE(store.ensureLoaded());

  JsonDocument current;
  current["cfgVersion"] = 3;
  current["username"] = "reader";
  current["password_obf"] = "secret";
  current["serverUrl"] = "https://sync.crosspointreader.com";
  current["matchMethod"] = 0;
  current["sendMetadata"] = false;
  current["syncBehavior"] = 0;
  current["customServers"].to<JsonArray>();
  ASSERT_TRUE(store.fromJson(current.as<JsonVariantConst>()));

  ASSERT_TRUE(store.addCustomServer("https://one.example"));
  EXPECT_EQ(store.getBaseUrl(), "https://one.example");
  ASSERT_EQ(store.getCustomServers().size(), 1U);

  Storage.resetIoCounters();
  ASSERT_TRUE(store.updateCustomServer(0, "https://one.example/"));
  EXPECT_EQ(Storage.openReadAttemptsFor(KOReaderCredentialStore::getFilePath()), 0U);
  EXPECT_EQ(Storage.openWriteAttemptsFor("/.crosspoint/koreader.json.tmp"), 0U);
  EXPECT_EQ(store.getCustomServers().front(), "https://one.example");

  ASSERT_TRUE(store.updateCustomServer(0, "two.example"));
  EXPECT_EQ(store.getBaseUrl(), "http://two.example");
  EXPECT_EQ(store.getCustomServers().front(), "two.example");

  Storage.makeUnwritable("/.crosspoint/koreader.json.tmp");
  EXPECT_FALSE(store.updateCustomServer(0, "https://failed.example"));
  EXPECT_EQ(store.getBaseUrl(), "http://two.example");
  EXPECT_EQ(store.getCustomServers().front(), "two.example");
  EXPECT_FALSE(store.removeCustomServer(0));
  EXPECT_EQ(store.getBaseUrl(), "http://two.example");
  EXPECT_EQ(store.getCustomServers().size(), 1U);
  Storage.makeWritable("/.crosspoint/koreader.json.tmp");

  EXPECT_TRUE(store.removeCustomServer(0));
  EXPECT_EQ(store.getBaseUrl(), "https://sync.crosspointreader.com");
  EXPECT_TRUE(store.getCustomServers().empty());
  EXPECT_FALSE(store.addCustomServer("https://sync.crosspointreader.com"));
  EXPECT_FALSE(store.addCustomServer(std::string(KOReaderCredentialStore::MAX_SERVER_URL_BYTES + 1, 'x')));
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

TEST(NetworkStore, RecentBooksRollBackFailedWritesSoTheSameUpdateCanRetry) {
  Storage.reset();
  auto& store = RECENT_BOOKS;
  ASSERT_TRUE(store.ensureLoaded());

  store.addBook("/Original.epub", "Original", "Author", "/original.bmp");
  ASSERT_EQ(store.getBooks().size(), 1U);
  ASSERT_EQ(store.getBooks().front().path, "/Original.epub");

  Storage.resetIoCounters();
  store.updateBook("/Original.epub", "Original", "Author", "/original.bmp");
  EXPECT_EQ(Storage.openReadAttemptsFor(RecentBooksStore::getFilePath()), 0U);
  EXPECT_EQ(Storage.openWriteAttemptsFor("/.crosspoint/recent.json.tmp"), 0U);

  Storage.makeUnwritable("/.crosspoint/recent.json.tmp");
  store.addBook("/Failed.epub", "Failed", "Author", "/failed.bmp");
  ASSERT_EQ(store.getBooks().size(), 1U);
  EXPECT_EQ(store.getBooks().front().path, "/Original.epub");

  Storage.makeWritable("/.crosspoint/recent.json.tmp");
  store.addBook("/Failed.epub", "Failed", "Author", "/failed.bmp");
  ASSERT_EQ(store.getBooks().size(), 2U);
  EXPECT_EQ(store.getBooks().front().path, "/Failed.epub");
  EXPECT_EQ(store.getBooks()[1].path, "/Original.epub");

  Storage.makeUnwritable("/.crosspoint/recent.json.tmp");
  store.addBook("/Original.epub", "Changed", "Other", "/changed.bmp");
  ASSERT_EQ(store.getBooks().size(), 2U);
  EXPECT_EQ(store.getBooks()[0].path, "/Failed.epub");
  EXPECT_EQ(store.getBooks()[1].path, "/Original.epub");
  EXPECT_EQ(store.getBooks()[1].title, "Original");
  EXPECT_EQ(store.getBooks()[1].author, "Author");
  EXPECT_EQ(store.getBooks()[1].coverBmpPath, "/original.bmp");
  Storage.makeWritable("/.crosspoint/recent.json.tmp");

  for (int i = 0; i < 8; ++i) {
    store.addBook("/Book" + std::to_string(i) + ".epub", "Book", "Author", "");
  }
  ASSERT_EQ(store.getBooks().size(), 10U);
  std::vector<std::string> pathsBeforeFailure;
  for (const RecentBook& book : store.getBooks()) pathsBeforeFailure.push_back(book.path);

  Storage.makeUnwritable("/.crosspoint/recent.json.tmp");
  store.addBook("/Overflow.epub", "Overflow", "Author", "");
  ASSERT_EQ(store.getBooks().size(), pathsBeforeFailure.size());
  for (size_t i = 0; i < pathsBeforeFailure.size(); ++i) {
    EXPECT_EQ(store.getBooks()[i].path, pathsBeforeFailure[i]);
  }

  Storage.makeWritable("/.crosspoint/recent.json.tmp");
  ASSERT_EQ(store.togglePin("/Original.epub"), RecentBooksStore::PinResult::Pinned);
  Storage.makeUnwritable("/.crosspoint/recent.json.tmp");
  store.updatePath("/Original.epub", "/Moved.epub", "/old-cache", "/new-cache");
  EXPECT_NE(std::find_if(store.getBooks().begin(), store.getBooks().end(),
                         [](const RecentBook& book) { return book.path == "/Original.epub"; }),
            store.getBooks().end());
  EXPECT_EQ(std::find_if(store.getBooks().begin(), store.getBooks().end(),
                         [](const RecentBook& book) { return book.path == "/Moved.epub"; }),
            store.getBooks().end());
  ASSERT_EQ(store.getPinnedPaths().size(), 1U);
  EXPECT_EQ(store.getPinnedPaths().front(), "/Original.epub");

  Storage.makeWritable("/.crosspoint/recent.json.tmp");
  store.updatePath("/Original.epub", "/Moved.epub", "/old-cache", "/new-cache");
  EXPECT_NE(std::find_if(store.getBooks().begin(), store.getBooks().end(),
                         [](const RecentBook& book) { return book.path == "/Moved.epub"; }),
            store.getBooks().end());
  ASSERT_EQ(store.getPinnedPaths().size(), 1U);
  EXPECT_EQ(store.getPinnedPaths().front(), "/Moved.epub");
}
