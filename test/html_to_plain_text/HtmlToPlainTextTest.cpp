#include <gtest/gtest.h>

#include <string>

#include "HtmlToPlainText.h"

TEST(HtmlToPlainText, SeparatesAllHeadingLevelsFromFollowingText) {
  for (int level = 1; level <= 6; level++) {
    const std::string tag = "h" + std::to_string(level);
    EXPECT_EQ(htmlToPlainText("<" + tag + ">Title</" + tag + ">Body"), "Title\n\nBody");
  }
}

TEST(HtmlToPlainText, PreservesExistingBlockBreaks) {
  EXPECT_EQ(htmlToPlainText("<p>First</p><div>Second</div>Third"), "First\n\nSecond\nThird");
}
