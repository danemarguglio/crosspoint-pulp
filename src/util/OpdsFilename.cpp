#include "OpdsFilename.h"

#include <cctype>

#include "StringUtils.h"

namespace {
bool isHex(const char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

int hexVal(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  return std::tolower(static_cast<unsigned char>(c)) - 'a' + 10;
}

// Local percent-decoder: FsHelpers::decodeUriEscapes exists but drags Arduino
// headers into this otherwise host-testable unit.
std::string percentDecode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '%' && i + 2 < in.size() && isHex(in[i + 1]) && isHex(in[i + 2])) {
      out += static_cast<char>((hexVal(in[i + 1]) << 4) | hexVal(in[i + 2]));
      i += 2;
    } else {
      out += in[i];
    }
  }
  return out;
}

bool endsWithEpub(const std::string& s) {
  static constexpr char EXT[] = ".epub";
  constexpr size_t n = sizeof(EXT) - 1;
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++) {
    if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != EXT[i]) return false;
  }
  return true;
}
}  // namespace

std::string opdsBookFilename(const std::string& author, const std::string& title, OpdsFilenameFormat format) {
  std::string base;
  switch (format) {
    case OpdsFilenameFormat::TitleAuthor:
      base = author.empty() ? title : title + " - " + author;
      break;
    case OpdsFilenameFormat::TitleOnly:
      base = title;
      break;
    case OpdsFilenameFormat::AuthorTitle:
    default:
      base = author.empty() ? title : author + " - " + title;
      break;
  }
  // sanitizeFilename caps at 100 bytes and never returns empty (falls back to
  // "book"); ".epub" is appended after so the extension is never truncated —
  // identical treatment to the previous inline construction.
  return StringUtils::sanitizeFilename(base) + ".epub";
}

std::string opdsServerFilename(const std::string& href) {
  std::string path = href;
  const size_t cut = path.find_first_of("?#");
  if (cut != std::string::npos) path.erase(cut);
  // Drop "scheme://authority" so a bare host never masquerades as a filename.
  const size_t scheme = path.find("://");
  if (scheme != std::string::npos) {
    const size_t pathStart = path.find('/', scheme + 3);
    if (pathStart == std::string::npos) return "";
    path.erase(0, pathStart);
  }
  while (!path.empty() && path.back() == '/') path.pop_back();
  const size_t slash = path.find_last_of('/');
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  if (base.empty()) return "";
  base = percentDecode(base);
  // Drop the extension before the 100-byte cap so it can never be truncated.
  if (endsWithEpub(base)) base.erase(base.size() - 5);
  return StringUtils::sanitizeFilename(base) + ".epub";
}
