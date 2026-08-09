#pragma once
#include <ArduinoJson.h>
#include <LazyStoreState.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

struct OpdsServer {
  std::string name;
  std::string url;
  std::string username;
  std::string password;  // Plaintext in memory; obfuscated with hardware key on disk
};

/**
 * Singleton class for storing OPDS server configurations on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON.
 */
class OpdsServerStore : public PersistableStore<OpdsServerStore> {
 private:
  std::vector<OpdsServer> servers;
  LazyStoreState loadState;

  static constexpr size_t MAX_SERVERS = 8;
  static constexpr size_t MAX_NAME_LENGTH = 63;
  static constexpr size_t MAX_URL_LENGTH = 127;
  static constexpr size_t MAX_USERNAME_LENGTH = 63;
  static constexpr size_t MAX_PASSWORD_LENGTH = 63;
  static bool validServer(const OpdsServer& server);

  OpdsServerStore() = default;

  friend class PersistableStore<OpdsServerStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/opds.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  bool loadFromFile();
  bool ensureLoaded();
  bool saveToFile() const;
  void markReadOnlyForRecovery();

  bool addServer(const OpdsServer& server);
  bool updateServer(size_t index, const OpdsServer& server);
  bool removeServer(size_t index);

  const std::vector<OpdsServer>& getServers() const {
    const_cast<OpdsServerStore*>(this)->ensureLoaded();
    return servers;
  }
  const OpdsServer* getServer(size_t index) const;
  size_t getCount() const {
    const_cast<OpdsServerStore*>(this)->ensureLoaded();
    return servers.size();
  }
  bool hasServers() const {
    const_cast<OpdsServerStore*>(this)->ensureLoaded();
    return !servers.empty();
  }
};

#define OPDS_STORE OpdsServerStore::getInstance()
