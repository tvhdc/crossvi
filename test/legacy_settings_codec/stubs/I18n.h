#pragma once

#include <cstring>

#include "I18nKeys.h"

class I18n {
 public:
  static I18n& getInstance() {
    static I18n instance;
    return instance;
  }

  const char* get(StrId) const { return ""; }

  static Language languageFromCode(const char* code) {
    if (!code) return Language::EN;
    for (uint8_t index = 0; index < getLanguageCount(); ++index) {
      if (std::strcmp(code, LANGUAGE_CODES[index]) == 0) return static_cast<Language>(index);
    }
    return Language::EN;
  }
};

#define I18N I18n::getInstance()
