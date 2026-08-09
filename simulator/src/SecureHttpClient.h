#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "HTTPClient.h"
#include "NetworkClient.h"
#include "WString.h"

namespace freeink {

class SecureHttpClient {
public:
  using DataCallback = std::function<bool(const uint8_t *, size_t)>;

  void setCACert(const char *) {}
  void setInsecure() {}

  bool begin(const String &url) {
    http_.begin(client_, url.c_str());
    contentLength_ = 0;
    haveContentLength_ = false;
    responseComplete_ = false;
    return !url.isEmpty();
  }

  bool begin(const std::string &url) { return begin(String(url)); }
  bool begin(const char *url) { return begin(String(url)); }

  void end() { http_.end(); }

  void addHeader(const char *name, const String &value) {
    http_.addHeader(name, value);
  }

  void addHeader(const char *name, const std::string &value) {
    http_.addHeader(name, value.c_str());
  }

  void addHeader(const char *name, const char *value) {
    http_.addHeader(name, value);
  }

  void setTimeout(uint16_t ms) { http_.setTimeout(ms); }
  void setReuse(bool reuse) { http_.setReuse(reuse); }

  int GET() { return http_.GET(); }
  int POST(const String &payload) { return http_.POST(payload.c_str()); }
  int sendRequest(const char *method, const String &payload) {
    if (method && std::string(method) == "PUT") {
      return http_.PUT(payload);
    }
    if (method && std::string(method) == "POST") {
      return http_.POST(payload.c_str());
    }
    return http_.GET();
  }
  int sendRequest(const char *method, const std::string &payload) {
    return sendRequest(method, String(payload));
  }
  int sendRequest(const char *method, const uint8_t *payload, size_t payloadLen,
                  const DataCallback &onData) {
    std::string body;
    if (payload && payloadLen > 0)
      body.assign(reinterpret_cast<const char *>(payload), payloadLen);
    const int status = sendRequest(method, body);
    const String response = http_.getString();
    contentLength_ = response.length();
    haveContentLength_ = status > 0;
    responseComplete_ = status > 0 && (!onData || onData(reinterpret_cast<const uint8_t *>(response.c_str()),
                                                         response.length()));
    return status;
  }

  String getString() { return http_.getString(); }
  int getSize() { return http_.getSize(); }
  bool responseComplete() const { return responseComplete_; }
  bool hasContentLength() const { return haveContentLength_; }
  size_t getContentLength() const { return contentLength_; }

  static bool tls13Available() { return true; }

private:
  NetworkClientSecure client_;
  HTTPClient http_;
  size_t contentLength_ = 0;
  bool haveContentLength_ = false;
  bool responseComplete_ = false;
};

} // namespace freeink
