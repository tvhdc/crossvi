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

TEST(KOReaderXPath, ResolvesExactVisibleOffsetAcrossNestedTextNodes) {
  const auto epub = std::make_shared<Epub>(
      "<html><head><title>Ignored</title></head><body><p>Alpha <em>beta</em> gamma</p>"
      "<script>ignored()</script><div>Delta</div></body></html>");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleOffset(epub, 0, 0), "/body/DocFragment[1]/body");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleOffset(epub, 0, 6),
            "/body/DocFragment[1]/body/p[1]/em[1]/text()[1].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleOffset(epub, 0, 8),
            "/body/DocFragment[1]/body/p[1]/em[1]/text()[1].2");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleOffset(epub, 0, 16),
            "/body/DocFragment[1]/body/div[1]/text()[1].0");
}

TEST(KOReaderXPath, ResolvesDirectBodyTextWithoutFallingBackToChapterStart) {
  const auto epub = std::make_shared<Epub>("<html><body>Lead<p>Paragraph</p></body></html>");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleOffset(epub, 0, 2), "/body/DocFragment[1]/body/text()[1].2");
}
