#include "HnTextLayout.h"

#include <Epub/ParsedText.h>
#include <Epub/blocks/BlockStyle.h>
#include <GfxRenderer.h>

namespace hn {

namespace {
bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
}  // namespace

void splitParagraphs(const std::string_view text, std::vector<std::string_view>& out) {
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    size_t a = pos;
    size_t b = end;
    while (a < b && isSpace(text[a])) a++;
    while (b > a && isSpace(text[b - 1])) b--;
    if (b > a) out.push_back(text.substr(a, b - a));
    pos = end + 1;
  }
}

bool layoutParagraph(const GfxRenderer& renderer, const int fontId, const int width, const std::string_view paragraph,
                     const bool justify, Lines& outLines) {
  if (width <= 0 || paragraph.empty()) return true;

  BlockStyle style;
  style.alignment = justify ? CssTextAlign::Justify : CssTextAlign::Left;
  style.textAlignDefined = true;
  // No extra paragraph spacing or focus reading here: the screens space
  // paragraphs themselves, and comments read better plain.
  ParsedText parsed(false, false, false, style);

  std::string word;
  word.reserve(24);
  for (size_t i = 0;; i++) {
    const bool atEnd = i >= paragraph.size();
    if (atEnd || isSpace(paragraph[i])) {
      if (!word.empty()) {
        parsed.addWord(word, EpdFontFamily::REGULAR);
        word.clear();
      }
      if (atEnd) break;
    } else {
      word.push_back(paragraph[i]);
    }
  }
  if (parsed.isEmpty()) return true;

  bool ok = true;
  parsed.layoutAndExtractLines(renderer, fontId, static_cast<uint16_t>(width),
                               [&outLines, &ok](std::unique_ptr<TextBlock> line, uint32_t) {
                                 if (!line || !line->valid()) {
                                   ok = false;
                                   return;
                                 }
                                 outLines.push_back(std::move(line));
                               });
  return ok;
}

}  // namespace hn
