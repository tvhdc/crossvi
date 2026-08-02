#include <ChapterXPathResolver.h>
#include <gtest/gtest.h>

#include <memory>

TEST(KOReaderXPath, ListItemsDoNotConsumeParagraphIndices) {
  const auto epub = std::make_shared<Epub>(
      "<html><body><ol><li>One</li><li>Two</li></ol>"
      "<p>First paragraph</p><div><p>Second paragraph</p></div></body></html>");

  EXPECT_EQ(ChapterXPathResolver::findXPathForParagraph(epub, 0, 1), "/body/DocFragment[1]/body/p[1]");
  EXPECT_EQ(ChapterXPathResolver::findXPathForParagraph(epub, 0, 2), "/body/DocFragment[1]/body/div[1]/p[1]");
  EXPECT_TRUE(ChapterXPathResolver::findXPathForParagraph(epub, 0, 3).empty());
}
