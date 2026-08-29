#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "DictionaryRegistry.h"

namespace {

void addDictionary(const std::string& name, const std::string& root = "/dictionaries") {
  const std::string folder = root + "/" + name;
  Storage.addDirectory(folder);
  Storage.setFile(folder + "/data.idx", {});
  Storage.setFile(folder + "/data.dict", {});
}

class DictionaryRegistryTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    Storage.addDirectory("/dictionaries");
  }
};

}  // namespace

TEST_F(DictionaryRegistryTest, DiscoversOnlyNamesThatRoundTripThroughSettings) {
  const std::string validUtf8 = "T\xE1\xBB\xAB \xC4\x91i\xE1\xBB\x83n";
  const std::string maxLength(31, 'a');
  addDictionary(validUtf8);
  addDictionary(maxLength);
  addDictionary(std::string(32, 'b'));
  addDictionary(std::string(140, 'c'));
  addDictionary(std::string("bad\xC3\x28", 5));

  std::vector<DictionaryEntry> dictionaries;
  DictionaryRegistry::discover(dictionaries);

  ASSERT_EQ(dictionaries.size(), 2U);
  EXPECT_EQ(dictionaries[0].name, maxLength);
  EXPECT_EQ(dictionaries[1].name, validUtf8);
}

TEST_F(DictionaryRegistryTest, ResolveRejectsUnrepresentableOrInvalidPersistedNames) {
  addDictionary("valid");

  std::string basePath;
  EXPECT_TRUE(DictionaryRegistry::resolveBasePath("valid", basePath));
  EXPECT_EQ(basePath, "/dictionaries/valid/data");
  EXPECT_FALSE(DictionaryRegistry::resolveBasePath(std::string(32, 'x').c_str(), basePath));
  EXPECT_FALSE(DictionaryRegistry::resolveBasePath(std::string("bad\xC3\x28", 5).c_str(), basePath));
}

TEST_F(DictionaryRegistryTest, DuplicateFolderAcrossRootsIsListedOnceWithPreferredRoot) {
  addDictionary("shared");
  addDictionary("shared", "/.dictionaries");

  std::vector<DictionaryEntry> dictionaries;
  DictionaryRegistry::discover(dictionaries);

  ASSERT_EQ(dictionaries.size(), 1U);
  EXPECT_EQ(dictionaries.front().name, "shared");

  std::string basePath;
  ASSERT_TRUE(DictionaryRegistry::resolveBasePath("shared", basePath));
  EXPECT_EQ(basePath, "/dictionaries/shared/data");
}

TEST_F(DictionaryRegistryTest, ResolveRejectsAStemFromAnIncompleteDirectoryScan) {
  const std::string folder = "/dictionaries/ambiguous";
  Storage.addDirectory(folder);
  Storage.setFile(folder + "/a.dict", {});
  Storage.setFile(folder + "/a.idx", {});
  Storage.setFile(folder + "/z.idx", {});
  Storage.failDirectoryIterationAfter(folder, 2);

  std::string basePath;
  EXPECT_FALSE(DictionaryRegistry::resolveBasePath("ambiguous", basePath));
}

TEST_F(DictionaryRegistryTest, DiscoveryDoesNotPublishAPartialRootScan) {
  addDictionary("first");
  addDictionary("second");
  Storage.failDirectoryIterationAfter("/dictionaries", 1);
  std::vector<DictionaryEntry> dictionaries{{"previous", "previous"}};

  DictionaryRegistry::discover(dictionaries);

  ASSERT_EQ(dictionaries.size(), 1U);
  EXPECT_EQ(dictionaries.front().name, "previous");
}
