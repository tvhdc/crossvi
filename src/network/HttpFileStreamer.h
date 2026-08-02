#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

struct HttpFileStreamResult {
  size_t bytesSent = 0;
  size_t expectedBytes = 0;

  [[nodiscard]] bool complete() const { return bytesSent == expectedBytes; }
};

template <typename File, typename Client, typename Service>
HttpFileStreamResult streamHttpFile(File& file, Client& client, uint8_t* buffer, const size_t bufferSize,
                                    Service&& service) {
  HttpFileStreamResult result{0, file.size()};
  if (buffer == nullptr || bufferSize == 0) return result;

  while (result.bytesSent < result.expectedBytes) {
    service();
    if (!client.connected()) return result;

    const size_t requested = std::min(bufferSize, result.expectedBytes - result.bytesSent);
    const int readResult = file.read(buffer, requested);
    if (readResult <= 0 || static_cast<size_t>(readResult) > requested) return result;

    const size_t bytesRead = static_cast<size_t>(readResult);
    size_t chunkSent = 0;
    while (chunkSent < bytesRead) {
      service();
      if (!client.connected()) return result;
      const size_t remaining = bytesRead - chunkSent;
      const size_t written = client.write(buffer + chunkSent, remaining);
      if (written == 0 || written > remaining) return result;
      chunkSent += written;
      result.bytesSent += written;
    }
  }

  return result;
}
