#include "SleepFrameStore.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <StagedFileTransaction.h>

namespace {
constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";
constexpr char SLEEP_FRAME_TEMP_FILE[] = "/.crosspoint/sleep_frame.bin.tmp";
constexpr char SLEEP_FRAME_BACKUP_FILE[] = "/.crosspoint/sleep_frame.bin.bak";
constexpr uint64_t X3_SLEEP_FRAME_BYTES = 792ULL * 528ULL / 8ULL;
constexpr uint64_t X4_SLEEP_FRAME_BYTES = 800ULL * 480ULL / 8ULL;

bool validateSleepFrameFile(const char* path, void* context) {
  const auto expectedSize = *static_cast<const size_t*>(context);
  HalFile file;
  if (!Storage.openFileForRead("SLP", path, file)) return false;
  const bool valid = file.fileSize64() == expectedSize;
  return file.close() && valid;
}
}  // namespace

namespace SleepFrameStore {

void discard() {
  Storage.remove(SLEEP_FRAME_TEMP_FILE);
  Storage.remove(SLEEP_FRAME_BACKUP_FILE);
  Storage.remove(SLEEP_FRAME_FILE);
}

bool save(const GfxRenderer& renderer) {
  const uint8_t* buffer = renderer.getFrameBuffer();
  const size_t bufferSize = renderer.getBufferSize();
  if (!buffer || bufferSize == 0) {
    Storage.remove(SLEEP_FRAME_TEMP_FILE);
    return false;
  }

  Storage.remove(SLEEP_FRAME_TEMP_FILE);
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_TEMP_FILE, file)) return false;
  const size_t written = file.write(buffer, bufferSize);
  bool saved = written == bufferSize;
  saved = file.sync() && saved;
  saved = file.close() && saved;
  if (!saved) {
    Storage.remove(SLEEP_FRAME_TEMP_FILE);
    return false;
  }

  StagedFileTransaction::Digest digest;
  StagedFileTransaction::updateDigest(digest, buffer, written);
  size_t expectedSize = bufferSize;
  const auto status = StagedFileTransaction::publishAndVerify(
      SLEEP_FRAME_FILE, SLEEP_FRAME_TEMP_FILE, SLEEP_FRAME_BACKUP_FILE, digest, validateSleepFrameFile, &expectedSize);
  if (status != StagedFileTransaction::Status::Published) Storage.remove(SLEEP_FRAME_TEMP_FILE);
  return status == StagedFileTransaction::Status::Published;
}

bool ready(const bool deviceIsX3) {
  size_t expectedSize = deviceIsX3 ? X3_SLEEP_FRAME_BYTES : X4_SLEEP_FRAME_BYTES;
  if (StagedFileTransaction::recover(SLEEP_FRAME_FILE, SLEEP_FRAME_BACKUP_FILE, validateSleepFrameFile,
                                     &expectedSize) == StagedFileTransaction::Status::IoError) {
    return false;
  }
  return validateSleepFrameFile(SLEEP_FRAME_FILE, &expectedSize);
}

bool load(HalDisplay& display, const bool consume) {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  if (file.fileSize64() != bufferSize) {
    file.close();
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }

  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  const bool closed = file.close();
  if (bytesRead != bufferSize || !closed) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  if (consume) Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

}  // namespace SleepFrameStore
