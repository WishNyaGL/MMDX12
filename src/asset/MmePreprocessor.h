#pragma once
// M1: the light C preprocessor used by the MME parser (separate so M2 can reuse the include resolution). It reads
// bytes (UTF-8 / cp932), strips comments (string literals respected), handles #include / #define / #undef /
// #if..#endif / #error / #pragma / #line, and expands macros in the surviving text. Every output line carries its
// original file:line so parser diagnostics point at the source that produced them.
//
// Deliberately not a full C preprocessor: no trigraphs, no variadic macros, no whole-file regex. It follows the
// subset MME effect files actually use (docs/mme/research/mme_spec_draft.md §A.1 macro-generated techniques).
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "asset/MmeEffect.h"

namespace mmdx {

// One output line's origin: which physical file and line it came from.
struct LineOrigin {
    std::filesystem::path file;
    int line = 0;
};

struct MmePreprocessResult {
    std::string text;                          // fully expanded text
    std::vector<LineOrigin> lines;             // one entry per line in `text`
    std::vector<std::filesystem::path> includes;   // resolved includes, first-seen order
    std::map<std::string, std::string> defines;    // final macro table
    std::vector<MmeDiag> diags;
    bool ok = false;                           // false only when the root file could not be read
};

// Preprocesses `fx`. `maxIncludeDepth` guards #include nesting (and macro recursion depth double this).
MmePreprocessResult PreprocessMme(const std::filesystem::path& fx, int maxIncludeDepth = 16,
                                  const std::map<std::string, std::string>& predefines = {});

} // namespace mmdx
