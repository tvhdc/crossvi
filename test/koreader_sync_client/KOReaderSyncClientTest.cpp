#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"

bool KOReaderCredentialStore::ensureLoaded() {
  loadState.markLoaded();
  return true;
}

void KOReaderCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
}

std::string KOReaderCredentialStore::getMd5Password() const { return "test-key"; }

bool KOReaderCredentialStore::hasCredentials() const { return !username.empty() && !password.empty(); }

void KOReaderCredentialStore::setServerUrl(const std::string& url) { serverUrl = url; }

std::string KOReaderCredentialStore::getBaseUrl() const { return serverUrl; }

bool KOReaderCredentialStore::usesCrossPointSyncServer() const { return false; }

namespace {

class KOReaderSyncClientTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() / ("crossvi-kosync-" + std::to_string(unique));
    ASSERT_TRUE(std::filesystem::create_directories(root_));
    ASSERT_EQ(setenv("CROSSPOINT_SIM_HTTP_MOCK_ROOT", root_.c_str(), 1), 0);
    KOREADER_STORE.setCredentials("reader", "secret");
    KOREADER_STORE.setServerUrl("https://sync.test");
  }

  void TearDown() override {
    unsetenv("CROSSPOINT_SIM_HTTP_MOCK_ROOT");
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  void setResponse(const std::string& document, const std::string& json) const {
    std::ofstream output(root_ / document, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output << json;
    ASSERT_TRUE(output.good());
  }

  static KOReaderProgress sentinelProgress() {
    KOReaderProgress progress{};
    progress.document = "unchanged-document";
    progress.progress = "unchanged-progress";
    progress.percentage = 0.75f;
    progress.device = "unchanged-device";
    progress.deviceId = "unchanged-id";
    progress.timestamp = 77;
    return progress;
  }

  static void expectSentinel(const KOReaderProgress& progress) {
    EXPECT_EQ(progress.document, "unchanged-document");
    EXPECT_EQ(progress.progress, "unchanged-progress");
    EXPECT_FLOAT_EQ(progress.percentage, 0.75f);
    EXPECT_EQ(progress.device, "unchanged-device");
    EXPECT_EQ(progress.deviceId, "unchanged-id");
    EXPECT_EQ(progress.timestamp, 77);
  }

 private:
  std::filesystem::path root_;
};

TEST_F(KOReaderSyncClientTest, AcceptsOfficialProgressResponse) {
  setResponse(
      "valid-document",
      R"({"document":"valid-document","progress":"/body/p[2]","percentage":0.25,"device":"KOReader","timestamp":123})");
  KOReaderProgress progress{};

  EXPECT_EQ(KOReaderSyncClient::getProgress("valid-document", progress), KOReaderSyncClient::OK);
  EXPECT_EQ(progress.document, "valid-document");
  EXPECT_EQ(progress.progress, "/body/p[2]");
  EXPECT_FLOAT_EQ(progress.percentage, 0.25f);
  EXPECT_EQ(progress.device, "KOReader");
  EXPECT_TRUE(progress.deviceId.empty());
  EXPECT_EQ(progress.timestamp, 123);
}

TEST_F(KOReaderSyncClientTest, CreatesUserThroughTheUnauthenticatedRequestPath) {
  setResponse("create", R"({"created":true})");

  EXPECT_EQ(KOReaderSyncClient::createUser(), KOReaderSyncClient::OK);
}

TEST_F(KOReaderSyncClientTest, EmptyOfficialResponseIsNotFoundAndLeavesOutputUntouched) {
  setResponse("missing-document", "{}");
  KOReaderProgress progress = sentinelProgress();

  EXPECT_EQ(KOReaderSyncClient::getProgress("missing-document", progress), KOReaderSyncClient::NOT_FOUND);
  expectSentinel(progress);
}

TEST_F(KOReaderSyncClientTest, RejectsInvalidRequiredFieldsWithoutMutatingOutput) {
  const std::vector<std::string> invalidResponses = {
      R"({"progress":12,"percentage":0.5})", R"({"progress":"/body/p[2]","percentage":"0.5"})",
      R"({"progress":"/body/p[2]","percentage":-0.1})", R"({"progress":"/body/p[2]","percentage":1.1})",
      R"({"progress":"","percentage":0.5})"};

  for (const auto& response : invalidResponses) {
    setResponse("invalid-document", response);
    KOReaderProgress progress = sentinelProgress();
    EXPECT_EQ(KOReaderSyncClient::getProgress("invalid-document", progress), KOReaderSyncClient::JSON_ERROR);
    expectSentinel(progress);
  }
}

TEST_F(KOReaderSyncClientTest, IgnoresMalformedCrossPointRichPosition) {
  KOREADER_STORE.setServerUrl(KOReaderCredentialStore::crossPointServerUrl());
  const std::vector<std::string> malformedPositions = {R"({})",
                                                       R"({"pctQ":"250000","spine":3,"page":7,"pages":8})",
                                                       R"({"pctQ":250000,"spine":3.0,"page":7,"pages":8})",
                                                       R"({"pctQ":250000,"spine":3,"page":-1,"pages":8})",
                                                       R"({"pctQ":250000,"spine":65536,"page":7,"pages":8})",
                                                       R"({"pctQ":1000001,"spine":3,"page":7,"pages":8})",
                                                       R"({"pctQ":250000,"spine":3,"page":7,"pages":0})",
                                                       R"({"pctQ":250000,"spine":3,"page":8,"pages":8})"};

  for (const auto& position : malformedPositions) {
    setResponse("malformed-position", R"({"progress":"/body/p[2]","percentage":0.25,"position":)" + position + "}");
    KOReaderProgress progress{};

    ASSERT_EQ(KOReaderSyncClient::getProgress("malformed-position", progress), KOReaderSyncClient::OK);
    EXPECT_EQ(progress.progress, "/body/p[2]");
    EXPECT_FLOAT_EQ(progress.percentage, 0.25f);
    EXPECT_FALSE(progress.position.has_value());
  }
}

TEST_F(KOReaderSyncClientTest, AcceptsValidCrossPointRichPosition) {
  KOREADER_STORE.setServerUrl(KOReaderCredentialStore::crossPointServerUrl());
  setResponse(
      "rich-position",
      R"({"progress":"/body/p[2]","percentage":0.25,"position":{"pctQ":250000,"spine":3,"page":7,"pages":8,"para":9,"xpath":"/body/p[2]"}})");
  KOReaderProgress progress{};

  ASSERT_EQ(KOReaderSyncClient::getProgress("rich-position", progress), KOReaderSyncClient::OK);
  ASSERT_TRUE(progress.position.has_value());
  EXPECT_EQ(progress.position->pctQ, 250000U);
  EXPECT_EQ(progress.position->spineIndex, 3U);
  EXPECT_EQ(progress.position->pageNumber, 7U);
  EXPECT_EQ(progress.position->totalPages, 8U);
  EXPECT_EQ(progress.position->paragraphIndex, 9U);
  EXPECT_EQ(progress.position->xpath, "/body/p[2]");
}

TEST_F(KOReaderSyncClientTest, IgnoresMalformedOptionalCrossPointRichPositionFields) {
  KOREADER_STORE.setServerUrl(KOReaderCredentialStore::crossPointServerUrl());
  const std::string oversizedXpath(121, 'x');
  setResponse(
      "malformed-rich-hints",
      R"({"progress":"/body/p[2]","percentage":0.25,"position":{"pctQ":250000,"spine":3,"page":7,"pages":8,"para":"9","xpath":")" +
          oversizedXpath + R"("}})");
  KOReaderProgress progress{};

  ASSERT_EQ(KOReaderSyncClient::getProgress("malformed-rich-hints", progress), KOReaderSyncClient::OK);
  ASSERT_TRUE(progress.position.has_value());
  EXPECT_FALSE(progress.position->paragraphIndex.has_value());
  EXPECT_TRUE(progress.position->xpath.empty());
}

TEST_F(KOReaderSyncClientTest, AcceptsMaximumLengthCrossPointRichPositionXpath) {
  KOREADER_STORE.setServerUrl(KOReaderCredentialStore::crossPointServerUrl());
  const std::string maxXpath(120, 'x');
  setResponse(
      "max-rich-xpath",
      R"({"progress":"/body/p[2]","percentage":0.25,"position":{"pctQ":250000,"spine":3,"page":7,"pages":8,"xpath":")" +
          maxXpath + R"("}})");
  KOReaderProgress progress{};

  ASSERT_EQ(KOReaderSyncClient::getProgress("max-rich-xpath", progress), KOReaderSyncClient::OK);
  ASSERT_TRUE(progress.position.has_value());
  EXPECT_EQ(progress.position->xpath, maxXpath);
}

}  // namespace
