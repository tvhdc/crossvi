#include "KOReaderCredentialStore.h"

#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>

namespace {
constexpr char DEFAULT_SERVER_URL[] = "https://sync.koreader.rocks:443";
constexpr char CROSSPOINT_SERVER_URL[] = "https://sync.crosspointreader.com";

// Bumped when a change to defaults would alter behavior for existing configs.
constexpr uint8_t CONFIG_VERSION = 3;
}  // namespace

void KOReaderCredentialStore::toJson(JsonDocument& doc) const {
  doc["cfgVersion"] = CONFIG_VERSION;
  doc["username"] = getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(getPassword());
  doc["serverUrl"] = getServerUrl();
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
  std::string user = doc["username"] | "";

  bool needsResave = false;
  std::string pass = extractPassword(doc, needsResave);

  setCredentials(user, pass);
  setServerUrl(doc["serverUrl"] | "");

  if (cfgVersion < CONFIG_VERSION) {
    // v2 briefly made crosspoint-sync the implicit default. Restore the
    // official KOReader server for every empty serverUrl; an explicitly saved
    // custom/CrossPoint URL remains unchanged.
    needsResave = true;
  }

  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  if (method <= static_cast<uint8_t>(DocumentMatchMethod::BINARY)) {
    setMatchMethod(static_cast<DocumentMatchMethod>(method));
  } else {
    LOG_DBG("KRS", "Invalid matchMethod %u in JSON, resetting to FILENAME", method);
    setMatchMethod(DocumentMatchMethod::FILENAME);
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
  if (!loaded && usable) {
    username.clear();
    password.clear();
    serverUrl.clear();
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
  username.clear();
  password.clear();
  saveToFile();
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
    url = DEFAULT_SERVER_URL;
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

bool KOReaderCredentialStore::usesCrossPointSyncServer() const { return getBaseUrl() == CROSSPOINT_SERVER_URL; }

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
