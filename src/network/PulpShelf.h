#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// One entry of pulp's GET /api/shelf: [{"file","url","size"}, ...] newest first.
struct PulpShelfItem {
  std::string file;  // server filename, e.g. "2026-09-18-some-article.epub"
  std::string url;   // absolute download URL
  uint32_t size = 0;
};

namespace pulp {

// Upper bound on entries kept; the shelf is the working set of one sync (a few
// dozen ~100-byte items), so this is a safety cap, not a target.
constexpr size_t MAX_SHELF_ITEMS = 400;

// GET <baseUrl>/api/shelf, streamed through the JSON parser into `out`.
// Returns false (and clears `out`) on HTTP or parse failure.
bool fetchShelf(const std::string& baseUrl, const std::string& username, const std::string& password,
                std::vector<PulpShelfItem>& out);

}  // namespace pulp
