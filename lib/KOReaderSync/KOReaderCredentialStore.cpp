#include "KOReaderCredentialStore.h"

#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace {
constexpr char LEGACY_DEFAULT_SERVER_URL[] = "https://sync.koreader.rocks:443";

// Bumped when a change to defaults would alter behavior for existing configs.
constexpr uint8_t CONFIG_VERSION = 3;

std::string normalizedUrl(std::string url) {
  if (url.find("://") == std::string::npos) url.insert(0, "http://");
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

bool sameUrl(const std::string& lhs, const std::string& rhs) { return normalizedUrl(lhs) == normalizedUrl(rhs); }
}  // namespace

void KOReaderCredentialStore::toJson(JsonDocument& doc) const {
  doc["cfgVersion"] = CONFIG_VERSION;
  doc["username"] = getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(getPassword());
  // Always write the effective URL. This keeps the selected endpoint stable
  // when downgrading to firmware whose implicit default was different.
  doc["serverUrl"] = getBaseUrl();
  JsonArray serverList = doc["customServers"].to<JsonArray>();
  const bool selectedIsCustom = !serverUrl.empty() && !isBuiltInServerUrl(serverUrl);
  const bool selectedAlreadySaved =
      selectedIsCustom && std::any_of(customServers.begin(), customServers.end(),
                                      [this](const auto& existing) { return sameUrl(existing, serverUrl); });
  const size_t savedLimit = selectedIsCustom && !selectedAlreadySaved && customServers.size() >= MAX_CUSTOM_SERVERS
                                ? MAX_CUSTOM_SERVERS - 1
                                : customServers.size();
  for (size_t i = 0; i < savedLimit; ++i) serverList.add(customServers[i]);
  if (selectedIsCustom && !selectedAlreadySaved) serverList.add(serverUrl);
  doc["matchMethod"] = static_cast<uint8_t>(getMatchMethod());
  doc["sendMetadata"] = getSendMetadata();
  doc["syncBehavior"] = static_cast<uint8_t>(getSyncBehavior());
}

bool KOReaderCredentialStore::fromJson(JsonVariantConst doc) {
  const uint8_t cfgVersion = doc["cfgVersion"] | (uint8_t)1;
  if (cfgVersion > CONFIG_VERSION) {
    LOG_ERR("KRS", "Credential config version %u is newer than supported version %u", cfgVersion, CONFIG_VERSION);
    return false;
  }
  bool needsResave = false;
  const char* storedUser = doc["username"] | "";
  const bool usernameValid = std::strlen(storedUser) <= MAX_USERNAME_BYTES;
  std::string user = usernameValid ? storedUser : "";

  bool passwordValid = false;
  std::string pass = extractPassword(doc, needsResave, MAX_PASSWORD_BYTES, passwordValid);
  if (!usernameValid || !passwordValid) {
    LOG_ERR("KRS", "Discarding oversized KOReader credentials");
    user.clear();
    pass.clear();
    needsResave = true;
  }

  const char* savedServer = doc["serverUrl"] | "";
  if (std::strlen(savedServer) > MAX_SERVER_URL_BYTES) return false;

  setCredentials(user, pass);

  customServers.clear();
  JsonArrayConst savedServers = doc["customServers"].as<JsonArrayConst>();
  customServers.reserve(std::min(savedServers.size(), MAX_CUSTOM_SERVERS));
  for (const char* saved : savedServers) {
    if (!saved || std::strlen(saved) == 0 || std::strlen(saved) > MAX_SERVER_URL_BYTES || isBuiltInServerUrl(saved)) {
      needsResave = true;
      continue;
    }
    const std::string server(saved);
    const bool duplicate = std::any_of(customServers.begin(), customServers.end(),
                                       [&server](const auto& existing) { return sameUrl(existing, server); });
    if (!duplicate && customServers.size() < MAX_CUSTOM_SERVERS) {
      customServers.push_back(server);
    } else {
      needsResave = true;
    }
  }

  serverUrl = savedServer;

  // Keep the implicit legacy endpoint when any credential data exists, but
  // let untouched stores adopt the new CrossPoint default.
  if (serverUrl.empty()) {
    if (!user.empty() || !pass.empty()) serverUrl = LEGACY_DEFAULT_SERVER_URL;
    needsResave = true;
  }

  if (!serverUrl.empty() && !isBuiltInServerUrl(serverUrl)) {
    const bool alreadySaved = std::any_of(customServers.begin(), customServers.end(),
                                          [this](const auto& existing) { return sameUrl(existing, serverUrl); });
    if (!alreadySaved) {
      if (customServers.size() >= MAX_CUSTOM_SERVERS) customServers.pop_back();
      customServers.push_back(serverUrl);
      needsResave = true;
    }
  }

  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  if (method <= static_cast<uint8_t>(DocumentMatchMethod::BINARY)) {
    setMatchMethod(static_cast<DocumentMatchMethod>(method));
  } else {
    LOG_DBG("KRS", "Invalid matchMethod %u in JSON, resetting to FILENAME", method);
    setMatchMethod(DocumentMatchMethod::FILENAME);
    needsResave = true;
  }
  setSendMetadata(doc["sendMetadata"] | false);

  const JsonVariantConst behaviorValue = doc["syncBehavior"];
  const bool missingBehavior = behaviorValue.isNull();
  uint8_t behavior = behaviorValue | static_cast<uint8_t>(KOReaderSyncBehavior::ASK_EVERY_TIME);
  if (behavior <= static_cast<uint8_t>(KOReaderSyncBehavior::SMART)) {
    setSyncBehavior(static_cast<KOReaderSyncBehavior>(behavior));
    needsResave = needsResave || missingBehavior;
  } else {
    LOG_DBG("KRS", "Invalid syncBehavior %u in JSON, resetting to ASK_EVERY_TIME", behavior);
    setSyncBehavior(KOReaderSyncBehavior::ASK_EVERY_TIME);
    needsResave = true;
  }

  if (needsResave) {
    LOG_DBG("KRS", "Resaved KOReader credentials to update format");
    saveToFile();
  }

  return true;
}

bool KOReaderCredentialStore::loadFromFile() {
  if (loadState.usable()) return true;
  if (loadState.failed() || !loadState.beginLoad()) return false;
  const bool loaded = PersistableStore<KOReaderCredentialStore>::loadFromFile();
  const bool usable = loaded || isPersistenceWritable();
  if (!loaded) {
    // Never expose stale or partially parsed credentials after a missing,
    // corrupt, newer, or unreadable store. Callers intentionally ignore the
    // lazy-load return value and read these fields directly.
    username.clear();
    password.clear();
    serverUrl.clear();
    customServers.clear();
    matchMethod = DocumentMatchMethod::FILENAME;
    sendMetadata = false;
    syncBehavior = KOReaderSyncBehavior::ASK_EVERY_TIME;
  }
  loadState.finish(usable);
  return usable;
}

bool KOReaderCredentialStore::ensureLoaded() { return loadFromFile(); }

bool KOReaderCredentialStore::saveToFile() const {
  if (!const_cast<KOReaderCredentialStore*>(this)->ensureLoaded()) return false;
  return PersistableStore<KOReaderCredentialStore>::saveToFile();
}

void KOReaderCredentialStore::markReadOnlyForRecovery() {
  username.clear();
  password.clear();
  serverUrl.clear();
  customServers.clear();
  loadState.markLoaded();
  PersistableStore<KOReaderCredentialStore>::markReadOnlyForRecovery();
}

void KOReaderCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  if (!ensureLoaded()) return;
  username = user;
  password = pass;
  LOG_DBG("KRS", "Set credentials for user: %s", user.c_str());
}

std::string KOReaderCredentialStore::getMd5Password() const {
  const_cast<KOReaderCredentialStore*>(this)->ensureLoaded();
  if (password.empty()) {
    return "";
  }

  // Calculate MD5 hash of password using ESP32's MD5Builder
  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();

  return md5.toString().c_str();
}

bool KOReaderCredentialStore::hasCredentials() const {
  const_cast<KOReaderCredentialStore*>(this)->ensureLoaded();
  return !username.empty() && !password.empty();
}

void KOReaderCredentialStore::clearCredentials() {
  if (!ensureLoaded()) return;
  std::string previousUsername = std::move(username);
  std::string previousPassword = std::move(password);
  username.clear();
  password.clear();
  if (!saveToFile()) {
    username = std::move(previousUsername);
    password = std::move(previousPassword);
    LOG_ERR("KRS", "Failed to clear KOReader credentials");
    return;
  }
  LOG_DBG("KRS", "Cleared KOReader credentials");
}

void KOReaderCredentialStore::setServerUrl(const std::string& url) {
  if (!ensureLoaded()) return;
  serverUrl = url;
  LOG_DBG("KRS", "Set server URL: %s", url.empty() ? "(default)" : url.c_str());
}

std::string KOReaderCredentialStore::getBaseUrl() const {
  const_cast<KOReaderCredentialStore*>(this)->ensureLoaded();
  std::string url;
  if (serverUrl.empty()) {
    url = crossPointServerUrl();
  } else if (serverUrl.find("://") == std::string::npos) {
    // Normalize URL: add http:// if no protocol specified (local servers typically don't have SSL)
    url = "http://" + serverUrl;
  } else {
    url = serverUrl;
  }

  // Strip trailing slashes to avoid double-slash in API paths
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }

  return url;
}

bool KOReaderCredentialStore::isBuiltInServerUrl(const std::string& url) {
  return sameUrl(url, crossPointServerUrl()) || sameUrl(url, koSyncServerUrl());
}

void KOReaderCredentialStore::rememberSelectedCustomServer() {
  if (serverUrl.empty() || isBuiltInServerUrl(serverUrl)) return;
  if (std::any_of(customServers.begin(), customServers.end(),
                  [this](const auto& existing) { return sameUrl(existing, serverUrl); })) {
    return;
  }
  if (customServers.size() >= MAX_CUSTOM_SERVERS) customServers.pop_back();
  customServers.push_back(serverUrl);
}

const std::vector<std::string>& KOReaderCredentialStore::getCustomServers() const {
  auto* self = const_cast<KOReaderCredentialStore*>(this);
  self->ensureLoaded();
  self->rememberSelectedCustomServer();
  return customServers;
}

bool KOReaderCredentialStore::selectServerUrl(const std::string& url) {
  if (!ensureLoaded() || url.empty() || url.size() > MAX_SERVER_URL_BYTES) return false;
  const std::string previous = serverUrl;
  serverUrl = sameUrl(url, crossPointServerUrl()) ? "" : url;
  if (previous == serverUrl) return true;
  if (saveToFile()) return true;
  serverUrl = previous;
  return false;
}

bool KOReaderCredentialStore::addCustomServer(const std::string& url) {
  if (!ensureLoaded() || url.empty() || url.size() > MAX_SERVER_URL_BYTES || isBuiltInServerUrl(url) ||
      customServers.size() >= MAX_CUSTOM_SERVERS) {
    return false;
  }
  if (std::any_of(customServers.begin(), customServers.end(),
                  [&url](const auto& existing) { return sameUrl(existing, url); })) {
    return false;
  }

  const std::string previousUrl = serverUrl;
  customServers.push_back(url);
  serverUrl = url;
  if (saveToFile()) return true;
  serverUrl = previousUrl;
  customServers.pop_back();
  return false;
}

bool KOReaderCredentialStore::updateCustomServer(const size_t index, const std::string& url) {
  if (!ensureLoaded() || index >= customServers.size() || url.empty() || url.size() > MAX_SERVER_URL_BYTES ||
      isBuiltInServerUrl(url)) {
    return false;
  }
  if (sameUrl(customServers[index], url)) return true;
  for (size_t i = 0; i < customServers.size(); ++i) {
    if (i != index && sameUrl(customServers[i], url)) return false;
  }

  const std::string previousUrl = serverUrl;
  std::string previousServer = std::move(customServers[index]);
  const bool selected = sameUrl(previousUrl.empty() ? crossPointServerUrl() : previousUrl, previousServer);
  customServers[index] = url;
  if (selected) serverUrl = url;
  if (saveToFile()) return true;
  serverUrl = previousUrl;
  customServers[index] = std::move(previousServer);
  return false;
}

bool KOReaderCredentialStore::removeCustomServer(const size_t index) {
  if (!ensureLoaded() || index >= customServers.size()) return false;
  const std::string previousUrl = serverUrl;
  std::string removed = std::move(customServers[index]);
  const bool selected = sameUrl(previousUrl.empty() ? crossPointServerUrl() : previousUrl, removed);
  customServers.erase(customServers.begin() + static_cast<ptrdiff_t>(index));
  if (selected) serverUrl.clear();
  if (saveToFile()) return true;
  serverUrl = previousUrl;
  customServers.insert(customServers.begin() + static_cast<ptrdiff_t>(index), std::move(removed));
  return false;
}

bool KOReaderCredentialStore::usesCrossPointSyncServer() const { return getBaseUrl() == crossPointServerUrl(); }

void KOReaderCredentialStore::setMatchMethod(DocumentMatchMethod method) {
  if (!ensureLoaded()) return;
  matchMethod = method;
  LOG_DBG("KRS", "Set match method: %s", method == DocumentMatchMethod::FILENAME ? "Filename" : "Binary");
}

void KOReaderCredentialStore::setSendMetadata(bool enabled) {
  if (!ensureLoaded()) return;
  sendMetadata = enabled;
  LOG_DBG("KRS", "Set send metadata: %s", enabled ? "true" : "false");
}

void KOReaderCredentialStore::setSyncBehavior(KOReaderSyncBehavior behavior) {
  if (!ensureLoaded()) return;
  if (static_cast<uint8_t>(behavior) > static_cast<uint8_t>(KOReaderSyncBehavior::SMART)) {
    behavior = KOReaderSyncBehavior::ASK_EVERY_TIME;
  }
  syncBehavior = behavior;
  LOG_DBG("KRS", "Set sync behavior: %s", behavior == KOReaderSyncBehavior::SMART ? "Smart" : "Ask");
}
