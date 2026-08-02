// ESP-IDF 5.5.2 asserts when realloc() fails while a fragmented response
// header is being assembled.  GitHub release redirects can produce headers
// large enough to hit that path on the ESP32-C3.  Keep the original allocation
// valid and let esp_http_client abort parsing normally instead of rebooting.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

char* __wrap_http_utils_append_string(char** str, const char* new_str, int len) {
  if (str == NULL) return NULL;

  char* old_str = *str;
  if (new_str == NULL) return old_str;

  const size_t append_len = len < 0 ? strlen(new_str) : (size_t)len;
  const size_t old_len = old_str == NULL ? 0 : strlen(old_str);
  if (old_len == SIZE_MAX || append_len > SIZE_MAX - old_len - 1) return NULL;

  char* grown = realloc(old_str, old_len + append_len + 1);
  if (grown == NULL) return NULL;

  memcpy(grown + old_len, new_str, append_len);
  grown[old_len + append_len] = '\0';
  *str = grown;
  return grown;
}
