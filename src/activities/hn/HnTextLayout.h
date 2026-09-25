#pragma once
#include <Epub/blocks/TextBlock.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class GfxRenderer;

// Pulp fork: plain-text layout for the Hacker News screens, built on the
// reader engine (ParsedText → TextBlock lines) so wrapping, hyphenation and
// justification match books instead of a second word-wrap implementation.
namespace hn {

using Lines = std::vector<std::unique_ptr<TextBlock>>;

// Splits text on runs of '\n' into trimmed, non-empty paragraphs (views into
// `text`; keep it alive while using them).
void splitParagraphs(std::string_view text, std::vector<std::string_view>& out);

// Lays one paragraph out into lines no wider than `width`. Words are split on
// ASCII whitespace; ParsedText handles NFC/CJK/RTL. Appends to `outLines`.
// Returns false when a line failed to allocate (the paragraph is cut there).
bool layoutParagraph(const GfxRenderer& renderer, int fontId, int width, std::string_view paragraph, bool justify,
                     Lines& outLines, bool hyphenate = false);

}  // namespace hn
