#pragma once

#include <expat.h>

#include <cstring>

// Safely tear down an expat parser: stop processing, clear callbacks, free, and null the pointer.
inline void destroyXmlParser(XML_Parser& parser) {
  if (!parser) return;
  XML_StopParser(parser, XML_FALSE);
  XML_SetElementHandler(parser, nullptr, nullptr);
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  parser = nullptr;
}

inline const char* xmlLocalName(const char* qualifiedName) {
  if (!qualifiedName) return "";
  const char* const separator = std::strchr(qualifiedName, ':');
  return separator ? separator + 1 : qualifiedName;
}

inline bool xmlLocalNameEquals(const char* qualifiedName, const char* expected) {
  return std::strcmp(xmlLocalName(qualifiedName), expected) == 0;
}
