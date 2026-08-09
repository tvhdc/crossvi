#include "DictionaryQuery.h"

#include <Utf8.h>

namespace DictionaryQuery {

std::string clean(const std::string_view text) {
  const std::string normalized = utf8CleanLookupWord(std::string(text));
  if (normalized.empty()) return {};

  std::string result;
  result.reserve(normalized.size());
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(normalized.c_str());
  while (*cursor) {
    utf8AppendCodepoint(utf8LowerVietnamese(utf8NextCodepoint(&cursor)), result);
  }
  return result;
}

bool buildPhrase(const char* const* words, const size_t count, std::string& out, const bool* joinWithoutSpaceBefore) {
  out.clear();
  if (!words || count == 0 || count > MAX_PHRASE_TOKENS) return false;

  for (size_t i = 0; i < count; i++) {
    const std::string token = clean(words[i] ? words[i] : "");
    if (token.empty()) return false;
    const size_t separator = out.empty() || (joinWithoutSpaceBefore && joinWithoutSpaceBefore[i]) ? 0 : 1;
    if (out.size() + separator + token.size() > MAX_QUERY_BYTES) {
      out.clear();
      return false;
    }
    if (separator) out.push_back(' ');
    out += token;
  }
  return true;
}

}  // namespace DictionaryQuery
