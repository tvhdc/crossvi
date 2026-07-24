#pragma once

#include <cstddef>
#include <cstdint>

#include <FontStorageContract.h>

namespace FontStorageUtils {

// Family names are persisted in CrossPointSettings::sdFontFamilyName[32].
// Paths also need room for the longest supported root, a hidden transaction
// directory, and a 96-byte .cpfont filename.
inline constexpr size_t MAX_FAMILY_NAME_BYTES = FontStorageContract::MAX_FAMILY_NAME_BYTES;
inline constexpr size_t MAX_CPFONT_FILENAME_BYTES = FontStorageContract::MAX_CPFONT_FILENAME_BYTES;
inline constexpr size_t FONT_PATH_CAPACITY = FontStorageContract::FONT_PATH_CAPACITY;

bool isValidFamilyName(const char* name);
bool isValidCpfontFilename(const char* name);
bool copyPersistedFamilyName(const char* name, char* output, size_t outputSize);
bool buildFamilyPath(const char* root, const char* family, char* output, size_t outputSize);
bool buildFontPath(const char* root, const char* family, const char* filename, char* output, size_t outputSize);
bool buildFilePath(const char* directory, const char* filename, char* output, size_t outputSize);
bool buildTransactionDirectoryPath(const char* root, const char* family, const char* suffix, char* output,
                                   size_t outputSize);

bool computeFileCrc32(const char* path, uint32_t& crc, uint64_t& size);

enum class FileMatch : uint8_t { Match, Different, IoError };
FileMatch fileMatches(const char* path, uint64_t expectedSize, uint32_t expectedCrc);

using FamilyValidator = bool (*)(const char* directory, void* context);

enum class FamilyTransactionStatus : uint8_t { Published, Recovered, NoRecoveryNeeded, InvalidStaging, IoError };

// Recover an interrupted family-level directory swap. A valid new directory
// wins only when it matches the current manifest; otherwise the untouched old
// directory is restored from backup.
FamilyTransactionStatus recoverFamily(const char* finalDirectory, const char* stagingDirectory,
                                      const char* backupDirectory, FamilyValidator newFamilyValidator,
                                      void* newFamilyContext, FamilyValidator oldFamilyValidator,
                                      void* oldFamilyContext);

// Atomically replace the complete family directory. The backup remains until
// every file in the published directory has passed newFamilyValidator.
FamilyTransactionStatus publishFamily(const char* finalDirectory, const char* stagingDirectory,
                                      const char* backupDirectory, FamilyValidator newFamilyValidator,
                                      void* newFamilyContext, FamilyValidator oldFamilyValidator,
                                      void* oldFamilyContext);

bool discardStagingFamily(const char* stagingDirectory);

}  // namespace FontStorageUtils
