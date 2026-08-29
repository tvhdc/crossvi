#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "HalDisplay.h"
#include "SleepFrameStore.h"
#include "SleepImageSelectionStore.h"

namespace {
constexpr char FRAME[] = "/.crosspoint/sleep_frame.bin";
constexpr char BACKUP[] = "/.crosspoint/sleep_frame.bin.bak";
constexpr char MANIFEST[] = "/.crosspoint/sleep_images.json";

GfxRenderer patternedRenderer(const int width, const int height, const uint8_t seed, const bool landscape = false) {
  GfxRenderer renderer(width, height, landscape);
  uint8_t* bytes = renderer.getFrameBuffer();
  for (size_t index = 0; index < renderer.getBufferSize(); ++index) {
    bytes[index] = static_cast<uint8_t>(seed + index * 17U);
  }
  return renderer;
}
}  // namespace

TEST(SleepFrameStoreTest, RoundTripsPhysicalX3AndX4GeometryAndHonorsConsume) {
  for (const auto& [width, height] : std::vector<std::pair<int, int>>{{528, 792}, {480, 800}}) {
    Storage.reset();
    ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
    GfxRenderer source = patternedRenderer(width, height, 3);
    ASSERT_TRUE(SleepFrameStore::save(source));
    EXPECT_TRUE(SleepFrameStore::ready(width == 528));

    HalDisplay first(width, height);
    ASSERT_TRUE(SleepFrameStore::load(first, false));
    EXPECT_TRUE(
        std::equal(source.getFrameBuffer(), source.getFrameBuffer() + source.getBufferSize(), first.getFrameBuffer()));
    EXPECT_TRUE(Storage.exists(FRAME));

    HalDisplay second(width, height);
    ASSERT_TRUE(SleepFrameStore::load(second));
    EXPECT_FALSE(Storage.exists(FRAME));
  }
}

TEST(SleepFrameStoreTest, RejectsSameSizeCorruptionAndLegacyRawFrames) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  GfxRenderer source = patternedRenderer(528, 792, 7);
  ASSERT_TRUE(SleepFrameStore::save(source));
  std::vector<uint8_t> corrupt = Storage.file(FRAME);
  corrupt.back() ^= 0x80U;
  Storage.setFile(FRAME, corrupt);
  EXPECT_FALSE(SleepFrameStore::ready(true));
  EXPECT_FALSE(Storage.exists(FRAME));

  Storage.setFile(FRAME, std::vector<uint8_t>(source.getBufferSize(), 0xAA));
  EXPECT_FALSE(SleepFrameStore::ready(true));
  EXPECT_FALSE(Storage.exists(FRAME));
}

TEST(SleepFrameStoreTest, RecoversValidBackupAndRejectsWrongModel) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  GfxRenderer source = patternedRenderer(528, 792, 11);
  ASSERT_TRUE(SleepFrameStore::save(source));
  const std::vector<uint8_t> valid = Storage.file(FRAME);
  std::vector<uint8_t> corrupt = valid;
  corrupt[corrupt.size() / 2] ^= 1U;
  Storage.setFile(FRAME, corrupt);
  Storage.setFile(BACKUP, valid);
  EXPECT_TRUE(SleepFrameStore::ready(true));
  EXPECT_FALSE(Storage.exists(BACKUP));
  EXPECT_EQ(Storage.file(FRAME), valid);
  EXPECT_FALSE(SleepFrameStore::ready(false));
}

TEST(SleepFrameStoreTest, SavesLandscapeFramebufferUsingFixedPanelGeometry) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  GfxRenderer source = patternedRenderer(528, 792, 19, true);
  ASSERT_TRUE(SleepFrameStore::save(source));
  EXPECT_TRUE(SleepFrameStore::ready(true));
}

TEST(SleepImageSelectionStoreTest, PublishesOneVerifiedOverlayFormatAtATime) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  Storage.setFile("/sleep-overlay.bmp.tmp", {'B', 'M', 'P'});
  ASSERT_TRUE(
      SleepImageSelectionStore::publish(SleepImageSelectionStore::Target::OverlayBmp, "/sleep-overlay.bmp.tmp"));
  ASSERT_TRUE(Storage.exists(SleepImageSelectionStore::OVERLAY_BMP_PATH));

  Storage.setFile("/sleep-overlay.png.tmp", {'P', 'N', 'G'});
  ASSERT_TRUE(
      SleepImageSelectionStore::publish(SleepImageSelectionStore::Target::OverlayPng, "/sleep-overlay.png.tmp"));
  EXPECT_TRUE(Storage.exists(SleepImageSelectionStore::OVERLAY_PNG_PATH));
  EXPECT_FALSE(Storage.exists(SleepImageSelectionStore::OVERLAY_BMP_PATH));
}

TEST(SleepImageSelectionStoreTest, RemovesMatchingStagingWhenCanonicalAlreadyHasSelectedBytes) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  const std::vector<uint8_t> image = {'P', 'S', 'A', 'M', 'E'};
  Storage.setFile(SleepImageSelectionStore::OVERLAY_PNG_PATH, image);
  Storage.setFile("/sleep-overlay.png.tmp", image);

  ASSERT_TRUE(
      SleepImageSelectionStore::publish(SleepImageSelectionStore::Target::OverlayPng, "/sleep-overlay.png.tmp"));
  EXPECT_EQ(Storage.file(SleepImageSelectionStore::OVERLAY_PNG_PATH), image);
  EXPECT_FALSE(Storage.exists("/sleep-overlay.png.tmp"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/sleep_image_selection_v1.pending"));
}

TEST(SleepImageSelectionStoreTest, ResumesPublicationAfterRenameFailure) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  Storage.setFile("/sleep-overlay.bmp", {'B', 'O', 'L', 'D'});
  Storage.setFile("/sleep-overlay.png.tmp", {'P', 'N', 'E', 'W'});
  Storage.failRenameTo("/sleep-overlay.png");
  EXPECT_FALSE(
      SleepImageSelectionStore::publish(SleepImageSelectionStore::Target::OverlayPng, "/sleep-overlay.png.tmp"));
  EXPECT_TRUE(Storage.exists("/.crosspoint/sleep_image_selection_v1.pending"));
  EXPECT_TRUE(Storage.exists("/sleep-overlay.bmp"));

  ASSERT_TRUE(SleepImageSelectionStore::recover());
  EXPECT_EQ(Storage.file("/sleep-overlay.png"), (std::vector<uint8_t>{'P', 'N', 'E', 'W'}));
  EXPECT_FALSE(Storage.exists("/sleep-overlay.bmp"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/sleep_image_selection_v1.pending"));
}

TEST(SleepImageSelectionStoreTest, RejectsInvalidStagingWithoutReplacingCurrentImage) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  Storage.setFile("/sleep-overlay.bmp", {'B', 'O', 'L', 'D'});
  Storage.setFile("/sleep-overlay.png.tmp", {'X', 'B', 'A', 'D'});
  EXPECT_FALSE(
      SleepImageSelectionStore::publish(SleepImageSelectionStore::Target::OverlayPng, "/sleep-overlay.png.tmp"));
  EXPECT_EQ(Storage.file("/sleep-overlay.bmp"), (std::vector<uint8_t>{'B', 'O', 'L', 'D'}));
  EXPECT_FALSE(Storage.exists("/.crosspoint/sleep_image_selection_v1.pending"));
}

TEST(SleepImageSelectionStoreTest, RemovesOrphanStagingFilesWithoutAPendingTransaction) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  Storage.setFile("/sleep.bmp.tmp", {'N'});
  Storage.setFile("/sleep-overlay.bmp.tmp", {'B'});
  Storage.setFile("/sleep-overlay.png.tmp", {'P'});

  ASSERT_TRUE(SleepImageSelectionStore::recover());
  EXPECT_FALSE(Storage.exists("/sleep.bmp.tmp"));
  EXPECT_FALSE(Storage.exists("/sleep-overlay.bmp.tmp"));
  EXPECT_FALSE(Storage.exists("/sleep-overlay.png.tmp"));
}

TEST(SleepImageSelectionStoreTest, MigratesLegacySelectionOnceWithStableIdsAndTransform) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  Storage.setFile("/sleep-overlay.png", {'P', 'R'});
  Storage.setFile("/sleep.bmp", {'N', 'R'});

  SleepImageSelectionStore::Catalog catalog;
  const SleepImageSelectionStore::ImageTransform legacy{125, -17, 9};
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(catalog, legacy), SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_TRUE(catalog.loaded);
  ASSERT_EQ(catalog.images.size(), 2U);
  EXPECT_EQ(catalog.images[0].id, 0x8000U);
  EXPECT_EQ(catalog.images[0].path, "/sleep-overlay.png");
  EXPECT_EQ(catalog.images[0].name, "sleep-overlay.png");
  EXPECT_EQ(catalog.images[0].transform.zoom, 125U);
  EXPECT_EQ(catalog.images[0].transform.offsetX, -17);
  EXPECT_EQ(catalog.images[0].transform.offsetY, 9);
  EXPECT_EQ(catalog.images[1].id, 0x8001U);
  EXPECT_EQ(catalog.images[1].path, "/sleep.bmp");
  EXPECT_EQ(catalog.images[1].transform.zoom, 125U);
  EXPECT_TRUE(Storage.exists(MANIFEST));

  ASSERT_TRUE(Storage.remove("/sleep-overlay.png"));
  Storage.setFile("/sleep-overlay.bmp", {'B', 'N'});
  SleepImageSelectionStore::Catalog reloaded;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(reloaded), SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_EQ(reloaded.images.size(), 2U);
  EXPECT_EQ(reloaded.images[0].id, 0x8000U);
  EXPECT_EQ(reloaded.images[0].path, "/sleep-overlay.png");
  EXPECT_EQ(reloaded.images[1].path, "/sleep.bmp");
}

TEST(SleepImageSelectionStoreTest, RecoversInterruptedRootPublicationBeforeOneTimeMigration) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  Storage.setFile("/sleep-overlay.png.bak", {'P', 'R'});

  SleepImageSelectionStore::Catalog catalog;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(catalog), SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_EQ(catalog.images.size(), 1U);
  EXPECT_EQ(catalog.images[0].path, "/sleep-overlay.png");
  EXPECT_TRUE(Storage.exists("/sleep-overlay.png"));
  EXPECT_FALSE(Storage.exists("/sleep-overlay.png.bak"));
}

TEST(SleepImageSelectionStoreTest, DoesNotCommitEmptyMigrationWhenLegacyDirectoryCannotBeOpened) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  Storage.setDirectory("/.sleep-overlay");
  Storage.makeUnreadable("/.sleep-overlay");

  SleepImageSelectionStore::Catalog catalog;
  EXPECT_EQ(SleepImageSelectionStore::loadCatalog(catalog), SleepImageSelectionStore::CatalogStatus::IoError);
  EXPECT_FALSE(catalog.loaded);
  EXPECT_FALSE(Storage.exists(MANIFEST));
}

TEST(SleepImageSelectionStoreTest, MigratesTheEffectiveLegacyFolderAndCapsTheCatalog) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  ASSERT_TRUE(Storage.mkdir("/.sleep-overlay"));
  for (int index = 0; index < 20; ++index) {
    Storage.setFile("/.sleep-overlay/image-" + std::to_string(index) + ".png", {'P', static_cast<uint8_t>(index)});
  }
  Storage.setFile("/.sleep-overlay/.hidden.png", {'P'});
  Storage.setFile("/.sleep-overlay/not-an-image.txt", {'P'});

  SleepImageSelectionStore::Catalog catalog;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(catalog), SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_EQ(catalog.images.size(), SleepImageSelectionStore::MAX_IMAGES);
  EXPECT_EQ(catalog.images.front().id, 0x8000U);
  EXPECT_EQ(catalog.images.back().id, 0x800FU);
  EXPECT_TRUE(std::all_of(catalog.images.begin(), catalog.images.end(),
                          [](const auto& entry) { return entry.path.rfind("/.sleep-overlay/image-", 0) == 0; }));
}

TEST(SleepImageSelectionStoreTest, AddUpdateAndRemovePersistPerImageState) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  SleepImageSelectionStore::Catalog catalog;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(catalog), SleepImageSelectionStore::CatalogStatus::Ok);

  Storage.setFile("/picked.png.tmp", {'P', 'N', 'G'});
  SleepImageSelectionStore::ImageEntry added;
  ASSERT_EQ(SleepImageSelectionStore::addPreparedImage(catalog, SleepImageSelectionStore::Target::OverlayPng,
                                                       "/picked.png.tmp", "picked.png", &added),
            SleepImageSelectionStore::CatalogStatus::Ok);
  EXPECT_EQ(added.id, 0x8000U);
  EXPECT_EQ(added.path, "/.sleep-overlay/crossvi-32768.png");
  EXPECT_TRUE(Storage.exists(added.path.c_str()));

  const SleepImageSelectionStore::ImageTransform transform{137, -31, 22};
  ASSERT_EQ(SleepImageSelectionStore::updateTransform(catalog, added.id, transform),
            SleepImageSelectionStore::CatalogStatus::Ok);
  SleepImageSelectionStore::Catalog reloaded;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(reloaded), SleepImageSelectionStore::CatalogStatus::Ok);
  const auto* persisted = SleepImageSelectionStore::findImage(reloaded, added.id);
  ASSERT_NE(persisted, nullptr);
  EXPECT_EQ(persisted->transform.zoom, 137U);
  EXPECT_EQ(persisted->transform.offsetX, -31);
  EXPECT_EQ(persisted->transform.offsetY, 22);

  ASSERT_EQ(SleepImageSelectionStore::removeImage(reloaded, added.id), SleepImageSelectionStore::CatalogStatus::Ok);
  EXPECT_FALSE(Storage.exists(added.path.c_str()));
  SleepImageSelectionStore::Catalog empty;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(empty), SleepImageSelectionStore::CatalogStatus::Ok);
  EXPECT_TRUE(empty.images.empty());
}

TEST(SleepImageSelectionStoreTest, RemovedManagedImageDoesNotReappearFromManifestBackup) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  SleepImageSelectionStore::Catalog catalog;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(catalog), SleepImageSelectionStore::CatalogStatus::Ok);

  Storage.setFile("/picked.png.tmp", {'P', 'N', 'G'});
  SleepImageSelectionStore::ImageEntry added;
  ASSERT_EQ(SleepImageSelectionStore::addPreparedImage(catalog, SleepImageSelectionStore::Target::OverlayPng,
                                                       "/picked.png.tmp", "picked.png", &added),
            SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_EQ(SleepImageSelectionStore::removeImage(catalog, added.id), SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_FALSE(Storage.exists(added.path.c_str()));

  Storage.setFile(MANIFEST, {'x'});
  SleepImageSelectionStore::Catalog recovered;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(recovered), SleepImageSelectionStore::CatalogStatus::Ok);
  EXPECT_TRUE(recovered.images.empty());
}

TEST(SleepImageSelectionStoreTest, FailedDeleteCheckpointRetainsManagedImageForTheOldBackup) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  SleepImageSelectionStore::Catalog catalog;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(catalog), SleepImageSelectionStore::CatalogStatus::Ok);

  Storage.setFile("/picked.png.tmp", {'P', 'N', 'G'});
  SleepImageSelectionStore::ImageEntry added;
  ASSERT_EQ(SleepImageSelectionStore::addPreparedImage(catalog, SleepImageSelectionStore::Target::OverlayPng,
                                                       "/picked.png.tmp", "picked.png", &added),
            SleepImageSelectionStore::CatalogStatus::Ok);

  Storage.resetIoCounters();
  Storage.failOpenReadOnAttempt(MANIFEST, 3);
  ASSERT_EQ(SleepImageSelectionStore::removeImage(catalog, added.id), SleepImageSelectionStore::CatalogStatus::Ok);
  EXPECT_TRUE(catalog.images.empty());
  EXPECT_TRUE(Storage.exists(added.path.c_str()));

  Storage.setFile(MANIFEST, {'x'});
  SleepImageSelectionStore::Catalog recovered;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(recovered), SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_EQ(recovered.images.size(), 1U);
  EXPECT_EQ(recovered.images[0].id, added.id);
  EXPECT_EQ(recovered.images[0].path, added.path);
  EXPECT_TRUE(Storage.exists(added.path.c_str()));
}

TEST(SleepImageSelectionStoreTest, KeepsIndependentTransformsAndOrderAcrossReloadAndDelete) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  SleepImageSelectionStore::Catalog catalog;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(catalog), SleepImageSelectionStore::CatalogStatus::Ok);

  std::array<SleepImageSelectionStore::ImageEntry, 3> added;
  for (size_t index = 0; index < added.size(); ++index) {
    const std::string staging = "/picked-" + std::to_string(index) + ".png.tmp";
    const std::string name = "picked-" + std::to_string(index) + ".png";
    Storage.setFile(staging, {'P', static_cast<uint8_t>(index)});
    ASSERT_EQ(SleepImageSelectionStore::addPreparedImage(catalog, SleepImageSelectionStore::Target::OverlayPng,
                                                         staging.c_str(), name, &added[index]),
              SleepImageSelectionStore::CatalogStatus::Ok);
  }
  ASSERT_EQ(SleepImageSelectionStore::updateTransform(catalog, added[0].id, {120, -10, 3}),
            SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_EQ(SleepImageSelectionStore::updateTransform(catalog, added[1].id, {135, 5, -8}),
            SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_EQ(SleepImageSelectionStore::updateTransform(catalog, added[2].id, {90, 14, 21}),
            SleepImageSelectionStore::CatalogStatus::Ok);

  SleepImageSelectionStore::Catalog reloaded;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(reloaded), SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_EQ(reloaded.images.size(), 3U);
  EXPECT_EQ(reloaded.images[0].id, added[0].id);
  EXPECT_EQ(reloaded.images[0].transform.zoom, 120U);
  EXPECT_EQ(reloaded.images[1].id, added[1].id);
  EXPECT_EQ(reloaded.images[1].transform.offsetY, -8);
  EXPECT_EQ(reloaded.images[2].id, added[2].id);
  EXPECT_EQ(reloaded.images[2].transform.offsetX, 14);

  ASSERT_EQ(SleepImageSelectionStore::updateTransform(reloaded, added[0].id, {150, -22, 11}),
            SleepImageSelectionStore::CatalogStatus::Ok);
  const auto* unchanged = SleepImageSelectionStore::findImage(reloaded, added[1].id);
  ASSERT_NE(unchanged, nullptr);
  EXPECT_EQ(unchanged->transform.zoom, 135U);
  EXPECT_EQ(unchanged->transform.offsetX, 5);
  EXPECT_EQ(unchanged->transform.offsetY, -8);
  ASSERT_EQ(SleepImageSelectionStore::removeImage(reloaded, added[1].id), SleepImageSelectionStore::CatalogStatus::Ok);

  SleepImageSelectionStore::Catalog finalCatalog;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(finalCatalog), SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_EQ(finalCatalog.images.size(), 2U);
  EXPECT_EQ(finalCatalog.images[0].id, added[0].id);
  EXPECT_EQ(finalCatalog.images[0].transform.zoom, 150U);
  EXPECT_EQ(finalCatalog.images[1].id, added[2].id);
  EXPECT_EQ(finalCatalog.images[1].transform.zoom, 90U);
}

TEST(SleepImageSelectionStoreTest, FailedManifestWritesRollBackCatalogAndManagedFiles) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  SleepImageSelectionStore::Catalog catalog;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(catalog), SleepImageSelectionStore::CatalogStatus::Ok);

  Storage.setFile("/picked.bmp.tmp", {'B', 'M', 'P'});
  Storage.failRenameTo(MANIFEST);
  EXPECT_EQ(SleepImageSelectionStore::addPreparedImage(catalog, SleepImageSelectionStore::Target::OverlayBmp,
                                                       "/picked.bmp.tmp", "picked.bmp"),
            SleepImageSelectionStore::CatalogStatus::IoError);
  EXPECT_TRUE(catalog.loaded);
  EXPECT_TRUE(catalog.images.empty());
  EXPECT_FALSE(Storage.exists("/.sleep-overlay/crossvi-32768.bmp"));
  SleepImageSelectionStore::Catalog reloaded;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(reloaded), SleepImageSelectionStore::CatalogStatus::Ok);
  EXPECT_TRUE(reloaded.images.empty());
}

TEST(SleepImageSelectionStoreTest, FailedUpdateAndRemoveRestoreThePublishedCatalog) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  Storage.setFile("/sleep-overlay.png", {'P'});
  SleepImageSelectionStore::Catalog catalog;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(catalog), SleepImageSelectionStore::CatalogStatus::Ok);
  const uint16_t id = catalog.images[0].id;

  Storage.failRenameTo(MANIFEST);
  EXPECT_EQ(SleepImageSelectionStore::updateTransform(catalog, id, {160, 10, -20}),
            SleepImageSelectionStore::CatalogStatus::IoError);
  EXPECT_EQ(catalog.images[0].transform.zoom, 100U);

  Storage.failRenameTo(MANIFEST);
  EXPECT_EQ(SleepImageSelectionStore::removeImage(catalog, id), SleepImageSelectionStore::CatalogStatus::IoError);
  ASSERT_EQ(catalog.images.size(), 1U);
  EXPECT_TRUE(Storage.exists("/sleep-overlay.png"));

  SleepImageSelectionStore::Catalog reloaded;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(reloaded), SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_EQ(reloaded.images.size(), 1U);
  EXPECT_EQ(reloaded.images[0].id, id);
  EXPECT_EQ(reloaded.images[0].transform.zoom, 100U);
}

TEST(SleepImageSelectionStoreTest, RemoveNeverDeletesAnExternalLegacyImage) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  Storage.setFile("/sleep-overlay.png", {'P'});
  SleepImageSelectionStore::Catalog catalog;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(catalog), SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_EQ(catalog.images.size(), 1U);
  ASSERT_EQ(SleepImageSelectionStore::removeImage(catalog, catalog.images[0].id),
            SleepImageSelectionStore::CatalogStatus::Ok);
  EXPECT_TRUE(Storage.exists("/sleep-overlay.png"));

  SleepImageSelectionStore::Catalog reloaded;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(reloaded), SleepImageSelectionStore::CatalogStatus::Ok);
  EXPECT_TRUE(reloaded.images.empty());
  EXPECT_TRUE(Storage.exists("/sleep-overlay.png"));
}

TEST(SleepImageSelectionStoreTest, RejectsMalformedManifestWithoutOverwritingIt) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  const std::string malformed =
      R"({"version":1,"nextId":32768,"images":[{"id":32768,"path":"/a.png","name":"a.png","zoom":100,"x":0,"y":0},{"id":32768,"path":"/b.png","name":"b.png","zoom":100,"x":0,"y":0}]})";
  Storage.setFile(MANIFEST, {malformed.begin(), malformed.end()});

  SleepImageSelectionStore::Catalog catalog;
  EXPECT_EQ(SleepImageSelectionStore::loadCatalog(catalog), SleepImageSelectionStore::CatalogStatus::Invalid);
  EXPECT_FALSE(catalog.loaded);
  EXPECT_EQ(std::string(Storage.file(MANIFEST).begin(), Storage.file(MANIFEST).end()), malformed);
}

TEST(SleepImageSelectionStoreTest, RecoversSemanticallyValidBackup) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  Storage.setFile("/sleep-overlay.png", {'P'});
  SleepImageSelectionStore::Catalog initial;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(initial), SleepImageSelectionStore::CatalogStatus::Ok);
  const std::vector<uint8_t> valid = Storage.file(MANIFEST);
  const std::string duplicateIds =
      R"({"version":1,"nextId":32768,"images":[{"id":32768,"path":"/a.png","name":"a.png","zoom":100,"x":0,"y":0},{"id":32768,"path":"/b.png","name":"b.png","zoom":100,"x":0,"y":0}]})";
  Storage.setFile(MANIFEST, {duplicateIds.begin(), duplicateIds.end()});
  Storage.setFile(std::string(MANIFEST) + ".bak", valid);

  SleepImageSelectionStore::Catalog recovered;
  ASSERT_EQ(SleepImageSelectionStore::loadCatalog(recovered), SleepImageSelectionStore::CatalogStatus::Ok);
  ASSERT_EQ(recovered.images.size(), 1U);
  EXPECT_EQ(recovered.images[0].path, "/sleep-overlay.png");
}
