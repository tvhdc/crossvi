#include <gtest/gtest.h>

#include "HttpTransportPolicy.h"

TEST(HttpTransportPolicy, AcceptsOnlyBoundedHttpUrlsWithoutUserInfo) {
  EXPECT_TRUE(HttpTransportPolicy::isSupportedUrl("https://api.github.com/repos/tvhdc/crossvi/releases/latest"));
  EXPECT_TRUE(HttpTransportPolicy::isSupportedUrl("http://192.168.1.2:8080/opds"));
  EXPECT_FALSE(HttpTransportPolicy::isSupportedUrl("ftp://example.com/book.epub"));
  EXPECT_FALSE(HttpTransportPolicy::isSupportedUrl("https://user:pass@example.com/book"));
  EXPECT_FALSE(HttpTransportPolicy::isSupportedUrl("https://example.com\\evil"));
  EXPECT_FALSE(HttpTransportPolicy::isSupportedUrl(std::string(HttpTransportPolicy::MAX_URL_BYTES + 1, 'a')));
}

TEST(HttpTransportPolicy, RecognizesHttpsForOta) {
  EXPECT_TRUE(HttpTransportPolicy::isHttpsUrl("https://github.com/tvhdc/crossvi/releases/download/v1/firmware.bin"));
  EXPECT_FALSE(HttpTransportPolicy::isHttpsUrl("http://github.com/tvhdc/crossvi/firmware.bin"));
  EXPECT_FALSE(HttpTransportPolicy::isHttpsUrl("HTTPS.example.com"));
}

TEST(HttpTransportPolicy, SendsCredentialsOnlyOverVerifiedTlsOrLocalHttp) {
  EXPECT_TRUE(HttpTransportPolicy::credentialsAllowed("https://books.example.com/opds"));
  EXPECT_TRUE(HttpTransportPolicy::credentialsAllowed("http://localhost:8080/opds"));
  EXPECT_TRUE(HttpTransportPolicy::credentialsAllowed("http://calibre.local/opds"));
  EXPECT_TRUE(HttpTransportPolicy::credentialsAllowed("http://10.1.2.3/opds"));
  EXPECT_TRUE(HttpTransportPolicy::credentialsAllowed("http://172.31.4.5/opds"));
  EXPECT_TRUE(HttpTransportPolicy::credentialsAllowed("http://192.168.1.10/opds"));
  EXPECT_FALSE(HttpTransportPolicy::credentialsAllowed("http://books.example.com/opds"));
  EXPECT_FALSE(HttpTransportPolicy::credentialsAllowed("http://172.32.4.5/opds"));
}

TEST(HttpTransportPolicy, RejectsDowngradesAndCrossOriginCredentialRedirects) {
  EXPECT_TRUE(
      HttpTransportPolicy::redirectAllowed("https://github.com/release", "https://objects.example/file", false));
  EXPECT_FALSE(
      HttpTransportPolicy::redirectAllowed("https://github.com/release", "http://objects.example/file", false));
  EXPECT_TRUE(
      HttpTransportPolicy::redirectAllowed("https://books.example:443/feed", "https://BOOKS.example/book", true));
  EXPECT_FALSE(HttpTransportPolicy::redirectAllowed("https://books.example/feed", "https://cdn.example/book", true));
  EXPECT_FALSE(HttpTransportPolicy::redirectAllowed("http://192.168.1.2/feed", "http://192.168.1.3/book", true));
}

TEST(HttpTransportPolicy, TreatsDefaultPortsAsTheSameOrigin) {
  EXPECT_TRUE(
      HttpTransportPolicy::redirectAllowed("https://books.example/feed", "https://books.example:443/book", true));
  EXPECT_TRUE(HttpTransportPolicy::redirectAllowed("http://books.local/feed", "http://BOOKS.local:80/book", true));
  EXPECT_FALSE(
      HttpTransportPolicy::redirectAllowed("https://books.example/feed", "https://books.example:444/book", true));
}

TEST(HttpTransportPolicy, RecognizesOnlyTheGithubReleaseAssetTlsHop) {
  EXPECT_TRUE(HttpTransportPolicy::isGithubReleaseAssetRedirect(
      "https://github.com/tvhdc/crossvi/releases/download/v1/firmware.bin",
      "https://release-assets.githubusercontent.com/github-production-release-asset/file?sig=x"));
  EXPECT_FALSE(HttpTransportPolicy::isGithubReleaseAssetRedirect("https://evil.example/file",
                                                                 "https://release-assets.githubusercontent.com/file"));
  EXPECT_FALSE(HttpTransportPolicy::isGithubReleaseAssetRedirect(
      "https://github.com/file", "https://release-assets.githubusercontent.com.evil.example/file"));
  EXPECT_FALSE(HttpTransportPolicy::isGithubReleaseAssetRedirect("http://github.com/file",
                                                                 "https://release-assets.githubusercontent.com/file"));
}
