#include <Utf8.h>
#include <gtest/gtest.h>

#include <string>

#include "OpdsParser.h"
#include "OpdsStream.h"

namespace {

void parse(OpdsParser& parser, const std::string& xml) {
  EXPECT_EQ(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  parser.flush();
  EXPECT_FALSE(parser.error());
}

TEST(OpdsParserTest, MatchesExactXmlLocalNamesOnly) {
  const std::string xml = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<atom:feed xmlns:atom="http://www.w3.org/2005/Atom" xmlns:dc="urn:test">
  <dc:entryLink>
    <atom:title>Not an entry</atom:title>
    <atom:link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="bad.epub"/>
  </dc:entryLink>
  <atom:entry>
    <atom:title>Good book</atom:title>
    <atom:author><atom:name>Good author</atom:name></atom:author>
    <atom:link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="good.epub"/>
  </atom:entry>
</atom:feed>)XML";

  OpdsParser parser;
  parse(parser, xml);

  ASSERT_EQ(parser.getEntries().size(), 1U);
  EXPECT_EQ(parser.getEntries()[0].title, "Good book");
  EXPECT_EQ(parser.getEntries()[0].author, "Good author");
  EXPECT_EQ(parser.getEntries()[0].href, "good.epub");
}

TEST(OpdsParserTest, TruncatesTextAtACompleteUtf8Codepoint) {
  const std::string longTitle = std::string(159, 'a') + "Ã©";
  const std::string xml = "<feed><entry><title>" + longTitle +
                          "</title><link rel=\"http://opds-spec.org/acquisition\" type=\"application/epub+zip\" "
                          "href=\"book.epub\"/></entry></feed>";

  OpdsParser parser;
  parse(parser, xml);

  ASSERT_EQ(parser.getEntries().size(), 1U);
  EXPECT_TRUE(utf8IsValid(parser.getEntries()[0].title.c_str()));
  EXPECT_EQ(parser.getEntries()[0].title, std::string(159, 'a'));
}

TEST(OpdsParserStreamTest, UnsupportedReadOperationsReturnEndOfStream) {
  OpdsParser parser;
  OpdsParserStream stream(parser);
  EXPECT_EQ(stream.available(), 0);
  EXPECT_EQ(stream.peek(), -1);
  EXPECT_EQ(stream.read(), -1);
}

}  // namespace
