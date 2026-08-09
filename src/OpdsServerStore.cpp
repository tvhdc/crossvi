#include "OpdsServerStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>

bool OpdsServerStore::validServer(const OpdsServer& server) {
  return server.name.size() <= MAX_NAME_LENGTH && server.url.size() <= MAX_URL_LENGTH &&
         server.username.size() <= MAX_USERNAME_LENGTH && server.password.size() <= MAX_PASSWORD_LENGTH;
}

void OpdsServerStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["servers"].to<JsonArray>();
  for (const auto& server : servers) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = server.name;
    obj["url"] = server.url;
    obj["username"] = server.username;
    obj["password_obf"] = obfuscation::obfuscateToBase64(server.password);
  }
}

bool OpdsServerStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'servers' key (treat as empty list); only a
  // JSON parse error is fatal. A null JsonArray iterates zero times.
  servers.clear();
  JsonArrayConst arr = doc["servers"].as<JsonArrayConst>();
  servers.reserve(std::min(arr.size(), MAX_SERVERS));
  bool needsResave = false;

  for (JsonObjectConst obj : arr) {
    if (servers.size() >= OpdsServerStore::MAX_SERVERS) break;
    const char* name = obj["name"] | "";
    const char* url = obj["url"] | "";
    const char* username = obj["username"] | "";
    if (std::strlen(name) > MAX_NAME_LENGTH || std::strlen(url) > MAX_URL_LENGTH ||
        std::strlen(username) > MAX_USERNAME_LENGTH) {
      needsResave = true;
      continue;
    }

    OpdsServer server;
    server.name = name;
    server.url = url;
    server.username = username;
    bool passwordValid = false;
    server.password = extractPassword(obj, needsResave, MAX_PASSWORD_LENGTH, passwordValid);
    if (!passwordValid) {
      needsResave = true;
      continue;
    }
    servers.push_back(std::move(server));
  }

  LOG_DBG("OPS", "Loaded %zu OPDS servers from file", servers.size());

  if (needsResave) {
    LOG_DBG("OPS", "Resaving JSON with obfuscated passwords");
    saveToFile();
  }

  return true;
}

bool OpdsServerStore::loadFromFile() {
  if (loadState.usable()) return true;
  if (loadState.failed() || !loadState.beginLoad()) return false;
  const bool loaded = PersistableStore<OpdsServerStore>::loadFromFile();
  const bool usable = loaded || isPersistenceWritable();
  if (!loaded && usable) servers.clear();
  loadState.finish(usable);
  return usable;
}

bool OpdsServerStore::ensureLoaded() { return loadFromFile(); }

bool OpdsServerStore::saveToFile() const {
  if (!const_cast<OpdsServerStore*>(this)->ensureLoaded()) return false;
  return PersistableStore<OpdsServerStore>::saveToFile();
}

void OpdsServerStore::markReadOnlyForRecovery() {
  servers.clear();
  loadState.markLoaded();
  PersistableStore<OpdsServerStore>::markReadOnlyForRecovery();
}

bool OpdsServerStore::addServer(const OpdsServer& server) {
  if (!ensureLoaded()) return false;
  if (!validServer(server) || servers.size() >= MAX_SERVERS) {
    LOG_DBG("OPS", "Cannot add more servers, limit of %zu reached", MAX_SERVERS);
    return false;
  }

  servers.push_back(server);
  LOG_DBG("OPS", "Added server: %s", server.name.c_str());
  if (saveToFile()) return true;
  servers.pop_back();
  return false;
}

bool OpdsServerStore::updateServer(size_t index, const OpdsServer& server) {
  if (!ensureLoaded()) return false;
  if (index >= servers.size() || !validServer(server)) {
    return false;
  }

  OpdsServer previous = std::move(servers[index]);
  servers[index] = server;
  LOG_DBG("OPS", "Updated server: %s", server.name.c_str());
  if (saveToFile()) return true;
  servers[index] = std::move(previous);
  return false;
}

bool OpdsServerStore::removeServer(size_t index) {
  if (!ensureLoaded()) return false;
  if (index >= servers.size()) {
    return false;
  }

  OpdsServer removed = std::move(servers[index]);
  LOG_DBG("OPS", "Removed server: %s", removed.name.c_str());
  servers.erase(servers.begin() + static_cast<ptrdiff_t>(index));
  if (saveToFile()) return true;
  servers.insert(servers.begin() + static_cast<ptrdiff_t>(index), std::move(removed));
  return false;
}

const OpdsServer* OpdsServerStore::getServer(size_t index) const {
  if (!const_cast<OpdsServerStore*>(this)->ensureLoaded()) return nullptr;
  if (index >= servers.size()) {
    return nullptr;
  }
  return &servers[index];
}
