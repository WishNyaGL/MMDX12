#pragma once
#include <filesystem>

namespace mmdx {

// True only when the volume holding `path` reports a seek penalty (a spinning disk).
// Any failure or unknown (network share, UNC path, spanned volume, access denied) returns
// false, i.e. today's behaviour.
bool PathIncursSeekPenalty(const std::filesystem::path& path);

} // namespace mmdx
