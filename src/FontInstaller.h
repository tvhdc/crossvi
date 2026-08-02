#pragma once

#include <SdCardFontRegistry.h>

#include <cstddef>
#include <cstdint>

#include "FontStorageUtils.h"

/// Shared utility for font installation (device download + browser upload).
/// Handles directory creation, file validation, deletion, and registry refresh.
class FontInstaller {
 public:
  enum class Error {
    OK,
    INVALID_FAMILY_NAME,
    INVALID_FILE,
    SD_WRITE_ERROR,
    MAX_FAMILIES_REACHED,
  };

  explicit FontInstaller(SdCardFontRegistry& registry);

  /// Validate a bounded family name: at most 31 alphanumeric, hyphen or underscore
  /// characters, with no path traversal.
  static bool isValidFamilyName(const char* name);

  /// Validate a .cpfont filename: ends with ".cpfont", no path separators or
  /// traversal sequences, basename uses only alphanumeric + hyphen + underscore
  /// + dot (only as the extension separator). Rejects "../foo.cpfont" and
  /// "evil/foo.cpfont". The complete filename is capped at 96 bytes so the
  /// fixed-size device paths cannot be truncated into a different destination.
  static bool isValidCpfontFilename(const char* name);

  /// Ensure /<root>/<family>/ exists, where <root> is /.fonts (preferred) or /fonts.
  /// Re-uses the existing root if the family is already installed; otherwise
  /// creates it under SdCardFontRegistry::defaultWriteRoot().
  bool ensureFamilyDir(const char* familyName);

  /// Ensure the selected top-level font root exists.
  static bool ensureRootDir(const char* root);

  /// Validate a .cpfont file using the bounded production parser.
  bool validateCpfontFile(const char* path);

  /// Validate every .cpfont in a family directory with the production parser.
  bool validateFamilyDirectory(const char* directory);

  /// Recover an interrupted catalog update for one family. This only reads
  /// font files when hidden transaction artifacts exist.
  bool recoverInterruptedFamilyDownload(const char* familyName);

  /// Build the full SD path for a font file.
  /// Writes "/<root>/<family>/<filename>" to outBuf, choosing <root> the same
  /// way ensureFamilyDir does (existing install dir, else default-write root).
  static bool buildFontPath(const char* family, const char* filename, char* outBuf, size_t outBufSize);

  /// Resolve the root used for an existing family, or the default write root.
  static const char* rootForFamily(const char* family);

  static bool buildFamilyPathAtRoot(const char* root, const char* family, char* outBuf, size_t outBufSize);
  static bool buildFontPathAtRoot(const char* root, const char* family, const char* filename, char* outBuf,
                                  size_t outBufSize);

  /// Delete a family directory and all .cpfont files in it.
  /// If the deleted family is the active reader font, clears the setting.
  Error deleteFamily(const char* familyName);

  /// Re-run registry discovery to pick up new/removed fonts.
  void refreshRegistry();

  /// Check whether a family name already exists in the registry.
  bool isFamilyInstalled(const char* familyName) const;

 private:
  SdCardFontRegistry& registry_;
};
