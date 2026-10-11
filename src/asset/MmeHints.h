#pragma once
// Follows the MikuMikuEffect references a model file carries, so a shader pack can find the model's own data (a
// face SDF, light maps, ...) without guessing folder names. The model's ".emd" sidecar (MME convention:
// same stem as the model) lists the effects it loads (".fx" files, paths relative to the .emd); each .fx and its
// #include chain declares textures with MME's ResourceName and a #define NAME "string"|number table. Nothing here
// depends on where the user keeps the model: only the references the asset files themselves contain are followed.
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mmdx {

struct MmeTexRef {
    std::string variable;          // the MME variable the texture was declared for
    std::filesystem::path file;    // existing file, resolved against the file that declared it
};

struct MmeHints {
    std::vector<MmeTexRef> textures;
    std::map<std::string, std::string> defines;   // NAME -> literal value (strings unquoted)
    bool found = false;                           // a .emd was read and at least one effect followed
};

// Reads the model's .emd ("<stem>.emd" next to the model), follows its [Effect] Obj / Obj[n] .fx entries and each
// #include chain (relative to the including file, then to the first .fx's folder; case-insensitive on Windows; depth
// <= 8; cycle-safe), collecting ResourceName textures and #define literals. Never throws; missing / unreadable files
// leave found = false and only log at [I]. Shift-JIS / UTF-8 tolerant.
MmeHints ReadMmeHints(const std::filesystem::path& modelFile);

} // namespace mmdx
