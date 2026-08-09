#include "WifiCredentialStore.h"

#include <CredentialIntegrity.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>

void WifiCredentialStore::toJson(JsonDocument& doc) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  doc["lastConnectedSsid"] = lastConnectedSsid;

  JsonArray arr = doc["credentials"].to<JsonArray>();
  for (const auto& cred : credentials) {
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = cred.ssid;
    obj["password_obf"] = obfuscation::obfuscateToBase64(cred.password);
    obj["password_len"] = cred.password.size();
    obj["password_crc32"] = credential_integrity::crc32(cred.password);
  }
}

bool WifiCredentialStore::fromJson(JsonVariantConst doc) {
  bool needsResave = false;
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    const char* persistedLastSsid = doc["lastConnectedSsid"] | "";
    if (std::strlen(persistedLastSsid) <= MAX_SSID_LENGTH) {
      lastConnectedSsid = persistedLastSsid;
    } else {
      lastConnectedSsid.clear();
      needsResave = true;
    }

    // Tolerate a missing/invalid 'credentials' key (treat as empty list); only
    // a JSON parse error is fatal. A null JsonArray iterates zero times.
    credentials.clear();
    JsonArrayConst arr = doc["credentials"].as<JsonArrayConst>();
    credentials.reserve(std::min(arr.size(), MAX_NETWORKS));
    for (JsonObjectConst obj : arr) {
      if (credentials.size() >= MAX_NETWORKS) break;
      WifiCredential cred;
      const char* ssid = obj["ssid"] | "";
      const size_t ssidLength = std::strlen(ssid);
      if (ssidLength == 0 || ssidLength > MAX_SSID_LENGTH) {
        needsResave = true;
        continue;
      }
      cred.ssid = ssid;

      const JsonVariantConst passwordLength = obj["password_len"];
      const bool hasPasswordLength = !passwordLength.isNull();
      size_t expectedLength = 0;
      if (hasPasswordLength) {
        if (!passwordLength.is<size_t>() || (expectedLength = passwordLength.as<size_t>()) > MAX_PASSWORD_LENGTH) {
          LOG_ERR("WCS", "Discarding invalid password length for %s", cred.ssid.c_str());
          needsResave = true;
          continue;
        }
      }

      bool passwordValid = false;
      cred.password = extractPassword(obj, needsResave, MAX_PASSWORD_LENGTH, passwordValid);
      if (!passwordValid) {
        LOG_ERR("WCS", "Discarding oversized password for %s", cred.ssid.c_str());
        needsResave = true;
        continue;
      }

      bool integrityValid = true;
      if (hasPasswordLength) {
        integrityValid = cred.password.size() == expectedLength;
      } else {
        needsResave = true;
      }

      const JsonVariantConst checksum = obj["password_crc32"];
      if (checksum.is<uint32_t>()) {
        integrityValid = integrityValid &&
                         credential_integrity::validate(cred.password, cred.password.size(), checksum.as<uint32_t>());
      } else if (checksum.isNull()) {
        needsResave = true;
      } else {
        integrityValid = false;
      }

      if (!integrityValid) {
        LOG_ERR("WCS", "Discarding corrupted password for %s", cred.ssid.c_str());
        needsResave = true;
        continue;
      }
      credentials.push_back(std::move(cred));
    }

    LOG_DBG("WCS", "Loaded %zu WiFi credentials from file", credentials.size());
  }

  if (needsResave) {
    LOG_DBG("WCS", "Resaving JSON with obfuscated passwords");
    resaveAfterLoad = true;
  }

  return true;
}

bool WifiCredentialStore::loadFromFile() {
  bool loaded = false;
  bool shouldResave = false;
  {
    std::lock_guard<std::mutex> lock(persistenceMutex);
    resaveAfterLoad = false;
    loaded = PersistableStore<WifiCredentialStore>::loadFromFile();
    shouldResave = loaded && resaveAfterLoad;
    resaveAfterLoad = false;
  }
  if (shouldResave && !saveToFile()) {
    LOG_ERR("WCS", "Failed to resave upgraded WiFi credentials");
  }
  return loaded;
}

bool WifiCredentialStore::saveToFile() const {
  std::lock_guard<std::mutex> lock(persistenceMutex);
  return PersistableStore<WifiCredentialStore>::saveToFile();
}

bool WifiCredentialStore::persistOrRestore(std::vector<WifiCredential>&& previousCredentials,
                                           std::string&& previousLastConnectedSsid) {
  if (PersistableStore<WifiCredentialStore>::saveToFile()) return true;
  std::lock_guard<std::mutex> lock(credentialMutex);
  credentials = std::move(previousCredentials);
  lastConnectedSsid = std::move(previousLastConnectedSsid);
  return false;
}

bool WifiCredentialStore::addCredential(const std::string& ssid, const std::string& password) {
  if (ssid.empty() || ssid.size() > MAX_SSID_LENGTH || password.size() > MAX_PASSWORD_LENGTH) return false;
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  std::vector<WifiCredential> previousCredentials;
  std::string previousLastConnectedSsid;
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    previousCredentials = credentials;
    previousLastConnectedSsid = lastConnectedSsid;
    const auto cred = find_if(credentials.begin(), credentials.end(),
                              [&ssid](const WifiCredential& credential) { return credential.ssid == ssid; });
    if (cred != credentials.end()) {
      cred->password = password;
      LOG_DBG("WCS", "Updated credentials for: %s", ssid.c_str());
    } else {
      if (credentials.size() >= MAX_NETWORKS) {
        LOG_DBG("WCS", "Cannot add more networks, limit of %zu reached", MAX_NETWORKS);
        return false;
      }
      credentials.push_back({ssid, password});
      LOG_DBG("WCS", "Added credentials for: %s", ssid.c_str());
    }
  }
  return persistOrRestore(std::move(previousCredentials), std::move(previousLastConnectedSsid));
}

bool WifiCredentialStore::updateCredential(const size_t index, const std::string& ssid, const std::string& password) {
  if (ssid.empty() || ssid.size() > MAX_SSID_LENGTH || password.size() > MAX_PASSWORD_LENGTH) return false;
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  std::vector<WifiCredential> previousCredentials;
  std::string previousLastConnectedSsid;
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    if (index >= credentials.size()) return false;
    previousCredentials = credentials;
    previousLastConnectedSsid = lastConnectedSsid;

    const std::string oldSsid = credentials[index].ssid;
    const auto existing = std::find_if(credentials.begin(), credentials.end(),
                                       [&ssid](const WifiCredential& item) { return item.ssid == ssid; });
    if (existing != credentials.end() && existing != credentials.begin() + static_cast<ptrdiff_t>(index)) {
      existing->password = password;
      credentials.erase(credentials.begin() + static_cast<ptrdiff_t>(index));
    } else {
      credentials[index] = {ssid, password};
    }
    if (oldSsid != ssid && lastConnectedSsid == oldSsid) lastConnectedSsid.clear();
  }
  return persistOrRestore(std::move(previousCredentials), std::move(previousLastConnectedSsid));
}

bool WifiCredentialStore::removeCredential(const std::string& ssid) {
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  std::vector<WifiCredential> previousCredentials;
  std::string previousLastConnectedSsid;
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    const auto cred = find_if(credentials.begin(), credentials.end(),
                              [&ssid](const WifiCredential& credential) { return credential.ssid == ssid; });
    if (cred == credentials.end()) return false;
    previousCredentials = credentials;
    previousLastConnectedSsid = lastConnectedSsid;
    credentials.erase(cred);
    LOG_DBG("WCS", "Removed credentials for: %s", ssid.c_str());
    if (ssid == lastConnectedSsid) lastConnectedSsid.clear();
  }
  return persistOrRestore(std::move(previousCredentials), std::move(previousLastConnectedSsid));
}

std::optional<WifiCredential> WifiCredentialStore::findCredential(const std::string& ssid) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  const auto cred = find_if(credentials.begin(), credentials.end(),
                            [&ssid](const WifiCredential& credential) { return credential.ssid == ssid; });
  return cred == credentials.end() ? std::nullopt : std::optional<WifiCredential>(*cred);
}

std::optional<WifiCredential> WifiCredentialStore::getCredentialAt(const size_t index) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  if (index >= credentials.size()) return std::nullopt;
  return credentials[index];
}

std::optional<std::string> WifiCredentialStore::getSsidAt(const size_t index) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  if (index >= credentials.size()) return std::nullopt;
  return credentials[index].ssid;
}

size_t WifiCredentialStore::getCredentialCount() const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  return credentials.size();
}

std::vector<WifiCredentialSummary> WifiCredentialStore::getCredentialSummaries() const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  std::vector<WifiCredentialSummary> summaries;
  summaries.reserve(credentials.size());
  for (const auto& credential : credentials) {
    summaries.push_back({credential.ssid, !credential.password.empty(), credential.ssid == lastConnectedSsid});
  }
  return summaries;
}

bool WifiCredentialStore::hasSavedCredential(const std::string& ssid) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  return find_if(credentials.begin(), credentials.end(),
                 [&ssid](const WifiCredential& credential) { return credential.ssid == ssid; }) != credentials.end();
}

void WifiCredentialStore::setLastConnectedSsid(const std::string& ssid) {
  if (ssid.size() > MAX_SSID_LENGTH) return;
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    if (lastConnectedSsid == ssid) return;
    lastConnectedSsid = ssid;
  }
  if (!PersistableStore<WifiCredentialStore>::saveToFile()) {
    LOG_ERR("WCS", "Failed to persist last connected SSID");
  }
}

std::string WifiCredentialStore::getLastConnectedSsid() const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  return lastConnectedSsid;
}

void WifiCredentialStore::clearLastConnectedSsid() {
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    if (lastConnectedSsid.empty()) return;
    lastConnectedSsid.clear();
  }
  if (!PersistableStore<WifiCredentialStore>::saveToFile()) {
    LOG_ERR("WCS", "Failed to clear last connected SSID");
  }
}

void WifiCredentialStore::clearAll() {
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    credentials.clear();
    lastConnectedSsid.clear();
  }
  if (!PersistableStore<WifiCredentialStore>::saveToFile()) {
    LOG_ERR("WCS", "Failed to clear WiFi credentials");
    return;
  }
  LOG_DBG("WCS", "Cleared all WiFi credentials");
}
