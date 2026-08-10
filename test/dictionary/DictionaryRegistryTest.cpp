#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "DictionaryRegistry.h"

namespace {

void addDictionary(const std::string& name) {
  const std::string folder = "/dictionaries/" + name;
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
