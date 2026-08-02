#pragma once

#include <expat.h>

inline void destroyXmlParser(XML_Parser& parser) {
  if (!parser) return;
  XML_ParserFree(parser);
  parser = nullptr;
}
