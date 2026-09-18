#include <gtest/gtest.h>

#include "FsHelpers.h"

namespace {

using namespace std::string_view_literals;

TEST(IsSafePathComponent, AcceptsNamesWithRepeatedDots) {
  EXPECT_TRUE(FsHelpers::isSafePathComponent("volume..2.epub"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent("notes...txt"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent(".hidden"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent("a.b"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent("book.epub"sv));
}

TEST(IsSafePathComponent, RejectsEmptyAndExactDotComponents) {
  EXPECT_FALSE(FsHelpers::isSafePathComponent(""sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("."sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent(".."sv));
}

TEST(IsSafePathComponent, RejectsPathSeparatorsAnywhereInTheComponent) {
  EXPECT_FALSE(FsHelpers::isSafePathComponent("a/b"sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("a\\b"sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("../x"sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("x/.."sv));
}

TEST(NormalisePath, CollapsesParentReferenceWithinPath) {
  EXPECT_EQ(FsHelpers::normalisePath("/Books/../.crosspoint/x"), ".crosspoint/x");
}

TEST(NormalisePath, DropsLeadingParentReferencesPastRoot) { EXPECT_EQ(FsHelpers::normalisePath("/../../etc"), "etc"); }

TEST(LeadingDateKey, ParsesIsoPrefixOnly) {
  EXPECT_EQ(FsHelpers::leadingDateKey("2026-09-18-some-article.epub"sv), 20260918u);
  EXPECT_EQ(FsHelpers::leadingDateKey("2026-09-18"sv), 20260918u);
  EXPECT_EQ(FsHelpers::leadingDateKey("2026-13-01-x.epub"sv), 0u);
  EXPECT_EQ(FsHelpers::leadingDateKey("2026-09-00-x.epub"sv), 0u);
  EXPECT_EQ(FsHelpers::leadingDateKey("foo-2026-09-18.epub"sv), 0u);
  EXPECT_EQ(FsHelpers::leadingDateKey("2026-09-1"sv), 0u);
  EXPECT_EQ(FsHelpers::leadingDateKey("20260918-x.epub"sv), 0u);
}

TEST(SortFileListNewestFirst, FoldersThenDateThenMtimeThenName) {
  std::vector<std::string> files = {"zeta.epub", "2026-09-17-b.epub", "b-dir/", "2026-09-18-a.epub", "beta.epub",
                                    "a-dir/",    "gamma.epub"};
  std::vector<uint32_t> mtimes = {500, 1, 0, 2, 900, 0, 500};
  FsHelpers::sortFileListNewestFirst(files, mtimes);
  const std::vector<std::string> expected = {"a-dir/",    "b-dir/",     "2026-09-18-a.epub", "2026-09-17-b.epub",
                                             "beta.epub", "gamma.epub", "zeta.epub"};
  EXPECT_EQ(files, expected);
  EXPECT_TRUE(mtimes.empty());
}

TEST(SortFileListNewestFirst, DatePrefixBeatsNewerMtime) {
  std::vector<std::string> files = {"undated.epub", "2020-01-01-old.epub"};
  std::vector<uint32_t> mtimes = {0xFFFFFFFFu, 0};
  FsHelpers::sortFileListNewestFirst(files, mtimes);
  EXPECT_EQ(files.front(), "2020-01-01-old.epub");
}

TEST(SortFileListNewestFirst, SizeMismatchFallsBackToAlphabetical) {
  std::vector<std::string> files = {"b.epub", "a.epub", "dir/"};
  std::vector<uint32_t> mtimes = {1};
  FsHelpers::sortFileListNewestFirst(files, mtimes);
  const std::vector<std::string> expected = {"dir/", "a.epub", "b.epub"};
  EXPECT_EQ(files, expected);
}

}  // namespace
