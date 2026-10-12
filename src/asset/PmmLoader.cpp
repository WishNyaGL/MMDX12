#include "asset/PmmLoader.h"
#include "asset/BinaryReader.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

// PMM layout sources (read before writing this file, not copied):
//   - tracebox.blog.fc2.com/blog-entry-35.html "MMDの新フォーマット解析" (Wayback 20160510082055):
//     the PMMv2 field tables for models, camera, light, accessory and the linked-list key frames.
//     It notes that the v1 layout differs only in the model header and that everything after the
//     camera "is unchanged".
//   - nanoem  nanoem/ext/document.c + document_p.h  ("PMMv2" parser; the same layout the
//     mmd-anim-format Rust crate cites). Used for the exact key-frame byte order and for the fields
//     the blog marks "Unknown": the accessory block (selected index + hscroll + a fixed count byte),
//     gravity / self-shadow (placed *after* the document frame settings, not before), the document
//     settings block, the edge colour and the model-selection footer.
//
// Exact record sizes (verified byte-for-byte against library/motion/Evergreen/Camera_PMM.pmm, which
// parses to the file's exact length):
//   base key frame     : [object index (additional keys only)] frame(i32) prev(i32) next(i32)
//   bone key           : base + 16 (4 channels x {x1,y1,x2,y2}) + 12 (t) + 16 (q) + 1 (sel) + 1 (physics,v2)
//   morph key          : base + 4 (weight) + 1 (sel)
//   camera key         : base + 4 (distance) + 12 + 12 + 8 (follow model/bone, v2) + 24 (6 params)
//                        + 1 (perspective) + 4 (fov) + 1 (sel)
//   light key          : base + 12 (color) + 12 (dir) + 1 (sel)
//   self-shadow key    : base + 1 (mode) + 4 (distance) + 1 (sel)
//   gravity key        : base + 1 (noise on) + 4 (noise) + 4 (accel) + 12 (dir) + 1 (sel)
//   accessory key      : base + 1 (packed opacity/visible) + 4 (parent model) + 4 (parent bone)
//                        + 12 (t) + 12 (euler degrees) + 4 (scale) + 1 (shadow) + 1 (sel)
//   model key          : base + 1 (visible) + Nc (constraint states) + 8*No (outside parents) + 1 (sel)
//   bone / morph state : 12 + 16 + 1 + 1 + 1  /  4
//
// The linked lists: each track has one (or, for bones/morphs, one per bone/morph) "initial" key with
// no object index, followed by N additional keys that DO carry an object index; every key names its
// next sibling by object index (0 = end). Initial keys are given object index 0..N-1 in file order;
// the additional keys keep the index stored in the file. Tracks are rebuilt by walking each initial
// key's next chain (see ReadTrack).

namespace mmdx {

namespace {

bool ContainsI(const std::string& s, const char* needle) {
    const size_t n = std::strlen(needle);
    if (n == 0 || s.size() < n) return false;
    for (size_t i = 0; i + n <= s.size(); ++i) {
        size_t j = 0;
        for (; j < n; ++j) {
            char a = s[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == n) return true;
    }
    return false;
}
bool StartsWithI(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}
std::string FileNameOf(const std::string& path) {
    const size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? path : path.substr(p + 1);
}

class Reader {
public:
    explicit Reader(BinaryReader& r) : r_(r) {}
    uint8_t U8() { uint8_t v = 0; r_.Read(v); return v; }
    int32_t I32() { int32_t v = 0; r_.Read(v); return v; }
    float F32() { float v = 0; r_.Read(v); return v; }
    XMFLOAT3 F3() { XMFLOAT3 v{}; r_.Read(v); return v; }
    XMFLOAT4 F4() { XMFLOAT4 v{0, 0, 0, 1}; r_.Read(v); return v; }
    void Skip(size_t n) { r_.Skip(n); }
    bool Ok() const { return !r_.Failed(); }
    size_t Remaining() const { return r_.Remaining(); }

    std::string VarString() {
        const int n = (int)U8();
        if (n <= 0) return {};
        std::string raw((size_t)n, '\0');
        r_.Read(raw.data(), (size_t)n);
        return SjisToUtf8(raw);
    }
    std::string FixedString(size_t bytes) {
        std::string raw(bytes, '\0');
        r_.Read(raw.data(), bytes);
        const size_t z = raw.find('\0');
        if (z != std::string::npos) raw.resize(z);
        return SjisToUtf8(raw);
    }

private:
    BinaryReader& r_;
};

bool IsPlausibleCount(int32_t n, size_t minRecordBytes, size_t remaining) {
    if (n < 0) return false;
    return (uint64_t)n * minRecordBytes <= remaining;
}

// Reads one track and rebuilds it into per-initial-key arrays (frame sorted).
//
// `read(r, indexed, key, objIndex, nextIndex)` consumes one record and reports the key's own object
// index (only meaningful for the additional keys, where `indexed` is true) and its next sibling.
// `numInitial` is the number of initial keys (bones / morphs count; 1 for the camera / light tracks).
template <class K, class ReadFn>
void ReadTrack(Reader& r, int numInitial, std::vector<K>* merged, std::vector<std::vector<K>>* perInitial,
               ReadFn read, std::vector<std::string>* warnings, const char* what) {
    struct Node {
        K key;
        int next = 0;
        bool used = false;
    };
    std::map<int, Node> nodes;
    for (int i = 0; i < numInitial; ++i) {
        K k;
        int obj = 0, next = 0;
        read(r, false, k, obj, next);
        if (!r.Ok()) return;
        nodes[i] = Node{k, next, false};
    }
    const int32_t count = r.I32();
    if (!IsPlausibleCount(count, 1, r.Remaining())) {
        if (count != 0 && warnings) warnings->push_back(std::string("PMM: ") + what + " key count looks corrupt");
        return;
    }
    std::set<int> seenIdx;
    for (int32_t j = 0; j < count; ++j) {
        K k;
        int obj = 0, next = 0;
        read(r, true, k, obj, next);
        if (!r.Ok()) break;
        // An additional key must not reuse an initial key's index (0..numInitial-1) or a previous
        // additional index: such a record is not part of any chain, so it is dropped.
        if (obj < numInitial || !seenIdx.insert(obj).second) continue;
        nodes[obj] = Node{k, next, false};
    }
    if (merged) merged->clear();
    if (perInitial) perInitial->assign((size_t)std::max(0, numInitial), {});
    for (int s = 0; s < numInitial; ++s) {
        std::vector<K>& dst = perInitial ? (*perInitial)[(size_t)s] : *merged;
        int cur = s;
        size_t guard = 0;
        while (cur >= 0 && guard++ <= nodes.size() + 1) {
            auto it = nodes.find(cur);
            if (it == nodes.end() || it->second.used) break;
            it->second.used = true;
            dst.push_back(it->second.key);
            cur = it->second.next;
        }
        std::sort(dst.begin(), dst.end(), [](const K& a, const K& b) { return a.frame < b.frame; });
    }
    if (!perInitial) {
        std::sort(merged->begin(), merged->end(), [](const K& a, const K& b) { return a.frame < b.frame; });
    }
}
} // namespace

bool LoadPmm(const std::filesystem::path& path, PmmDocument& out, std::string* error) {
    try {
        out = PmmDocument{};
        std::vector<uint8_t> data;
        if (!ReadWholeFile(path, data, error)) return false;
        if (data.size() < 46) { if (error) *error = "not a PMM file (too small)"; return false; }
        if (std::memcmp(data.data(), "Polygon Movie maker ", 20) != 0) {
            if (error) *error = "not a PMM file";
            return false;
        }
        {
            std::string ver(reinterpret_cast<const char*>(data.data() + 20), 4);
            const size_t z = ver.find('\0');
            if (z != std::string::npos) ver.resize(z);
            out.version = ver.empty() ? "0002" : ver;
        }
        const bool v2 = out.version != "0001";

        BinaryReader br(data.data(), data.size());
        Reader r(br);

        r.Skip(30);
        out.outputWidth = r.I32();
        out.outputHeight = r.I32();
        r.I32();                 // timeline (frame operation window) width
        out.timelineFov = r.F32();
        r.Skip(v2 ? 7 : 7);      // editing flag + panel expand flags
        const int32_t selectedModel = r.U8();
        const int32_t modelCount = r.U8();
        (void)selectedModel;
        if (!r.Ok()) { if (error) *error = "corrupt PMM header"; return false; }

        if (!v2) {
            // Recognised but not importable: PMM 0001 stores no model names / bone lists, and the
            // reference readers resynthesise them by loading the PMX the model path points at. This
            // loader is independent of PMX loading, so it declines rather than misreading the stream.
            if (error) *error = "PMM 0001 layout is not supported";
            return false;
        }

        for (int32_t mi = 0; mi < modelCount; ++mi) {
            PmmModel m;
            m.index = (int)r.U8();
            m.nameJa = r.VarString();
            m.nameEn = r.VarString();
            m.path = Utf8ToPath(r.FixedString(256));
            r.U8();  // num_fixed_tracks
            const int32_t numBones = r.I32();
            if (!IsPlausibleCount(numBones, 1, r.Remaining())) { if (error) *error = "corrupt PMM model"; return false; }
            for (int32_t i = 0; i < numBones; ++i) m.boneNames.push_back(r.VarString());
            const int32_t numMorphs = r.I32();
            if (!IsPlausibleCount(numMorphs, 1, r.Remaining())) { if (error) *error = "corrupt PMM model"; return false; }
            for (int32_t i = 0; i < numMorphs; ++i) m.morphNames.push_back(r.VarString());
            const int32_t numConstraints = r.I32();
            if (!IsPlausibleCount(numConstraints, 4, r.Remaining())) { if (error) *error = "corrupt PMM model"; return false; }
            for (int32_t i = 0; i < numConstraints; ++i) m.ikBones.push_back(r.I32());
            const int32_t numOutside = r.I32();
            if (!IsPlausibleCount(numOutside, 4, r.Remaining())) { if (error) *error = "corrupt PMM model"; return false; }
            for (int32_t i = 0; i < numOutside; ++i) m.outsideParentBones.push_back(r.I32());

            r.U8();                      // draw order index
            m.visible = r.U8() != 0;
            r.I32();                     // selected bone
            r.Skip(16);                  // 4 selected morph indices
            const int32_t numExpand = (uint8_t)r.U8();
            r.Skip((size_t)std::max(0, numExpand));
            r.I32();                     // vertical scroll
            m.lastFrame = r.I32();
            if (!r.Ok()) { if (error) *error = "corrupt PMM model"; return false; }

            // ---- bone keys (one initial key per bone + linked additional keys) ----
            std::vector<std::vector<PmmBoneKey>> boneTracks;
            ReadTrack<PmmBoneKey>(r, numBones, nullptr, &boneTracks,
                [](Reader& rr, bool indexed, PmmBoneKey& k, int& obj, int& next) {
                    obj = indexed ? rr.I32() : 0;
                    k.frame = rr.I32();
                    rr.I32();  // prev keyframe (chain is walked forward through next)
                    next = rr.I32();
                    for (int c = 0; c < 4; ++c) {   // PMM {x1,y1,x2,y2} per channel
                        k.interp[c * 4 + 0] = rr.U8();
                        k.interp[c * 4 + 1] = rr.U8();
                        k.interp[c * 4 + 2] = rr.U8();
                        k.interp[c * 4 + 3] = rr.U8();
                    }
                    k.translation = rr.F3();
                    k.rotation = rr.F4();
                    rr.U8();  // selected
                    k.physicsDisabled = rr.U8() != 0;
                }, &out.warnings, "bone");
            if (!r.Ok()) { if (error) *error = "corrupt PMM bone keys"; return false; }
            for (size_t bi = 0; bi < m.boneNames.size() && bi < boneTracks.size(); ++bi)
                if (!boneTracks[bi].empty()) m.bones[m.boneNames[bi]] = std::move(boneTracks[bi]);

            // ---- morph keys ----
            std::vector<std::vector<PmmMorphKey>> morphTracks;
            ReadTrack<PmmMorphKey>(r, numMorphs, nullptr, &morphTracks,
                [](Reader& rr, bool indexed, PmmMorphKey& k, int& obj, int& next) {
                    obj = indexed ? rr.I32() : 0;
                    k.frame = rr.I32();
                    rr.I32();  // prev
                    next = rr.I32();
                    k.weight = rr.F32();
                    rr.U8();   // selected
                }, &out.warnings, "morph");
            if (!r.Ok()) { if (error) *error = "corrupt PMM morph keys"; return false; }
            for (size_t bi = 0; bi < m.morphNames.size() && bi < morphTracks.size(); ++bi)
                if (!morphTracks[bi].empty()) m.morphs[m.morphNames[bi]] = std::move(morphTracks[bi]);

            // ---- model (visibility / IK / outside parent) keys ----
            std::vector<PmmModelKey> modelKeys;
            ReadTrack<PmmModelKey>(r, 1, &modelKeys, nullptr,
                [numConstraints, numOutside](Reader& rr, bool indexed, PmmModelKey& k, int& obj, int& next) {
                    obj = indexed ? rr.I32() : 0;
                    k.frame = rr.I32();
                    rr.I32();  // prev
                    next = rr.I32();
                    k.visible = rr.U8() != 0;
                    k.ikEnabled.resize((size_t)numConstraints);
                    for (int c = 0; c < numConstraints; ++c) k.ikEnabled[(size_t)c] = rr.U8();
                    k.outsideParent.resize((size_t)numOutside);
                    for (int c = 0; c < numOutside; ++c) k.outsideParent[(size_t)c] = {rr.I32(), rr.I32()};
                    k.selfShadow = rr.U8() != 0;
                }, &out.warnings, "model");
            if (!r.Ok()) { if (error) *error = "corrupt PMM model keys"; return false; }
            m.modelKeys = std::move(modelKeys);

            // ---- trailing per-model states ----
            r.Skip((size_t)numBones * 31);              // bone states (t12 + q16 + dirty + physics + selected)
            r.Skip((size_t)numMorphs * 4);              // morph states
            r.Skip((size_t)numConstraints);             // constraint states
            r.Skip((size_t)numOutside * 16);            // outside-parent states
            r.U8();                                      // blend enabled
            r.F32();                                     // edge width
            r.U8();                                      // self shadow enabled
            r.U8();                                      // transform order index
            if (!r.Ok()) { if (error) *error = "corrupt PMM model tail"; return false; }
            out.models.push_back(std::move(m));
        }

        // ---- camera ----
        ReadTrack<PmmCameraKey>(r, 1, &out.cameraKeys, nullptr,
            [](Reader& rr, bool indexed, PmmCameraKey& k, int& obj, int& next) {
                obj = indexed ? rr.I32() : 0;
                k.frame = rr.I32();
                rr.I32();  // prev
                next = rr.I32();
                k.distance = rr.F32();
                k.target = rr.F3();
                k.rotation = rr.F3();
                k.followModel = rr.I32();
                k.followBone = rr.I32();
                uint8_t c[6][4];
                for (int p = 0; p < 6; ++p)
                    for (int q = 0; q < 4; ++q) c[p][q] = rr.U8();
                for (int p = 0; p < 6; ++p) {  // {x1,y1,x2,y2} -> VMD {x1,x2,y1,y2}
                    k.interp[p * 4 + 0] = c[p][0];
                    k.interp[p * 4 + 1] = c[p][2];
                    k.interp[p * 4 + 2] = c[p][1];
                    k.interp[p * 4 + 3] = c[p][3];
                }
                k.perspective = rr.U8() == 0;
                k.fovDeg = (uint32_t)std::max(0, rr.I32());
                rr.U8();  // selected
            }, &out.warnings, "camera");
        if (!r.Ok()) { if (error) *error = "corrupt PMM camera keys"; return false; }
        r.Skip(37);  // camera state (3 x vector3 + perspective byte)

        // ---- light ----
        ReadTrack<PmmLightKey>(r, 1, &out.lightKeys, nullptr,
            [](Reader& rr, bool indexed, PmmLightKey& k, int& obj, int& next) {
                obj = indexed ? rr.I32() : 0;
                k.frame = rr.I32();
                rr.I32();  // prev
                next = rr.I32();
                k.color = rr.F3();
                k.direction = rr.F3();
                rr.U8();   // selected
            }, &out.warnings, "light");
        if (!r.Ok()) { if (error) *error = "corrupt PMM light keys"; return false; }
        r.Skip(24);  // light state (2 x vector3)

        // ---- accessory block ----
        r.U8();      // selected accessory index
        r.I32();     // accessory horizontal scroll
        const int32_t numAccessories = r.U8();
        if (numAccessories < 0) { if (error) *error = "corrupt PMM accessory block"; return false; }
        for (int32_t a = 0; a < numAccessories; ++a) r.FixedString(100);  // the name table (repeated in the body)
        if (!r.Ok()) { if (error) *error = "corrupt PMM accessory block"; return false; }
        for (int32_t a = 0; a < numAccessories; ++a) {
            PmmAccessory acc;
            r.U8();                        // accessory index
            acc.name = r.FixedString(100);
            acc.path = Utf8ToPath(r.FixedString(256));
            r.U8();                        // draw order index
            std::vector<PmmAccessoryKey> keys;
            ReadTrack<PmmAccessoryKey>(r, 1, &keys, nullptr,
                [](Reader& rr, bool indexed, PmmAccessoryKey& k, int& obj, int& next) {
                    obj = indexed ? rr.I32() : 0;
                    k.frame = rr.I32();
                    rr.I32();  // prev
                    next = rr.I32();
                    const uint8_t packed = rr.U8();
                    k.opacity = (float)(100 - ((packed & 0xfe) >> 1)) * 0.01f;
                    k.visible = (packed & 0x01) != 0;
                    k.parentModel = rr.I32();
                    k.parentBone = rr.I32();
                    k.translation = rr.F3();
                    k.rotationDeg = rr.F3();
                    k.scale = rr.F32();
                    rr.U8();   // shadow enabled
                    rr.U8();   // selected
                }, &out.warnings, "accessory");
            if (!r.Ok()) { if (error) *error = "corrupt PMM accessory keys"; return false; }
            acc.keys = std::move(keys);
            // accessory state (packed opacity/visible, parent, translation, scale, orientation, flags)
            r.Skip(1 + 4 + 4 + 12 + 4 + 12 + 1 + 1);
            out.accessories.push_back(std::move(acc));
        }
        if (!r.Ok()) { if (error) *error = "corrupt PMM accessory tail"; return false; }

        // ---- document settings ----
        r.I32();   // current frame
        r.I32();   // horizontal scroll
        r.I32();   // horizontal scroll thumb
        r.I32();   // editing mode
        r.U8();    // camera look mode
        out.loop = r.U8() != 0;
        const uint8_t beginEnabled = r.U8();
        const uint8_t endEnabled = r.U8();
        const int32_t beginFrame = r.I32();
        const int32_t endFrame = r.I32();
        out.audioEnabled = r.U8() != 0;
        out.audioPath = Utf8ToPath(r.FixedString(256));
        r.Skip(4 + 4 + 4);                 // background video offset x/y + scale
        r.FixedString(256);                // background video path
        r.Skip(4);                         // video enabled (i32)
        r.Skip(4 + 4 + 4);                 // background image offset x/y + scale
        r.FixedString(256);                // background image path
        r.U8();                            // image enabled
        r.U8();                            // information shown
        r.U8();                            // grid and axis shown
        r.U8();                            // ground shadow shown
        out.preferredFps = r.F32();
        r.I32();                           // screen capture mode
        r.I32();                           // accessory index after models
        r.F32();                           // ground shadow brightness
        r.U8();                            // translucent ground shadow enabled
        r.U8();                            // physics simulation mode
        if (beginEnabled && endEnabled && beginFrame >= 0 && endFrame >= beginFrame) {
            out.hasRange = true;
            out.rangeStart = (int)beginFrame;
            out.rangeEnd = (int)endFrame;
        }
        if (!r.Ok()) { if (error) *error = "corrupt PMM settings"; return false; }

        // ---- gravity ----
        r.F32();   // acceleration
        r.I32();   // noise
        r.F3();    // direction
        r.U8();    // noise enabled
        {
            struct GravityKey { int frame = 0; };
            std::vector<GravityKey> ignored;
            ReadTrack<GravityKey>(r, 1, &ignored, nullptr,
                [](Reader& rr, bool indexed, GravityKey& k, int& obj, int& next) {
                    obj = indexed ? rr.I32() : 0;
                    k.frame = rr.I32();     // frame
                    rr.I32();               // prev
                    next = rr.I32();
                    rr.U8(); rr.I32(); rr.F32(); rr.F3(); rr.U8();
                }, &out.warnings, "gravity");
        }

        // ---- self shadow ----
        r.U8();    // enabled
        r.F32();   // distance
        std::vector<std::vector<PmmShadowKey>> shadowTracks;
        ReadTrack<PmmShadowKey>(r, 1, &out.shadowKeys, nullptr,
            [](Reader& rr, bool indexed, PmmShadowKey& k, int& obj, int& next) {
                obj = indexed ? rr.I32() : 0;
                k.frame = rr.I32();
                rr.I32();  // prev
                next = rr.I32();
                k.mode = rr.U8();
                k.distance = rr.F32();
                rr.U8();   // selected
            }, &out.warnings, "self-shadow");
        if (!r.Ok()) { if (error) *error = "corrupt PMM tail"; return false; }

        // ---- edge colour / project flags / optional model-selection footer ----
        r.I32(); r.I32(); r.I32();  // edge colour RGB (0..255)
        r.U8();                     // black background
        r.I32();                    // camera look-at model
        r.I32();                    // camera look-at bone
        r.Skip(64);                 // unknown matrix
        r.U8();                     // following look-at enabled
        r.U8();                     // unknown boolean
        r.U8();                     // physics ground enabled
        r.I32();                    // current frame in text field
        if (!r.Ok()) {
            out.warnings.push_back("PMM: truncated document tail");
        }

        // ---- max frame ----
        int maxFrame = 0;
        for (const PmmModel& m : out.models) {
            for (const auto& [n, keys] : m.bones) if (!keys.empty()) maxFrame = std::max(maxFrame, keys.back().frame);
            for (const auto& [n, keys] : m.morphs) if (!keys.empty()) maxFrame = std::max(maxFrame, keys.back().frame);
            if (!m.modelKeys.empty()) maxFrame = std::max(maxFrame, m.modelKeys.back().frame);
        }
        if (!out.cameraKeys.empty()) maxFrame = std::max(maxFrame, out.cameraKeys.back().frame);
        if (!out.lightKeys.empty()) maxFrame = std::max(maxFrame, out.lightKeys.back().frame);
        if (!out.shadowKeys.empty()) maxFrame = std::max(maxFrame, out.shadowKeys.back().frame);
        if (out.hasRange) maxFrame = std::max(maxFrame, out.rangeEnd);
        out.maxFrame = maxFrame;

        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    } catch (...) {
        if (error) *error = "unexpected exception";
        return false;
    }
}

// ---------------------------------------------------------------------------
// MME effect controllers
// ---------------------------------------------------------------------------

MmeController ClassifyMmeController(const std::string& fileName, std::string* matchedName) {
    const std::string n = FileNameOf(fileName);
    if (n.empty()) return MmeController::None;
    const auto set = [&](MmeController c) { if (matchedName) *matchedName = n; return c; };
    // The ~10 most common controller families (one table). Order matters (first match wins): the more
    // specific names come first. A normal accessory model (a .pmx the user placed) is not matched.
    if (StartsWithI(n, "autoluminous")) return set(MmeController::AutoLuminous);
    if (StartsWithI(n, "diffusion")) return set(MmeController::Diffusion);
    if (StartsWithI(n, "excellentshadow") || ContainsI(n, "hgshadow")) return set(MmeController::Shadow);
    if (StartsWithI(n, "ikbokeh") || ContainsI(n, "svdof") || ContainsI(n, "hgdof") ||
        StartsWithI(n, "dof") || ContainsI(n, "dof_controller"))
        return set(MmeController::Dof);
    if (StartsWithI(n, "ssao") || ContainsI(n, "hgsao") || StartsWithI(n, "sao"))
        return set(MmeController::Ssao);
    if (StartsWithI(n, "o_tonemap") || StartsWithI(n, "tonemap")) return set(MmeController::Tone);
    if (StartsWithI(n, "o_lightbloom") || StartsWithI(n, "o_glow") || StartsWithI(n, "lightbloom"))
        return set(MmeController::LightBloom);
    if (StartsWithI(n, "o_posteffect") || StartsWithI(n, "o_selfoverlay") || StartsWithI(n, "o_surplus") ||
        StartsWithI(n, "o_objectluminous"))
        return set(MmeController::PostEffect);
    if (n == "ray.x" || StartsWithI(n, "ray_controller") || StartsWithI(n, "ray-mmd") ||
        StartsWithI(n, "ray.") || ContainsI(n, "ray_") )
        return set(MmeController::Ray);
    return MmeController::None;
}

const char* MmeControllerName(MmeController c) {
    switch (c) {
        case MmeController::AutoLuminous: return "AutoLuminous";
        case MmeController::Diffusion: return "Diffusion";
        case MmeController::Dof: return "DoF";
        case MmeController::Ssao: return "SSAO";
        case MmeController::Tone: return "Tone";
        case MmeController::Shadow: return "Shadow";
        case MmeController::LightBloom: return "LightBloom";
        case MmeController::PostEffect: return "PostEffect";
        case MmeController::Ray: return "Ray-MMD";
        case MmeController::None: default: return "None";
    }
}

bool LooksLikeUnknownMmeController(const std::filesystem::path& path) {
    const std::string n = ToLowerAscii(PathToUtf8(path.filename()));
    if (n.empty()) return false;
    if (ClassifyMmeController(n) != MmeController::None) return false;
    const bool isEffectModel = n.size() > 2 && (n.substr(n.size() - 2) == ".x" || n.substr(n.size() - 4) == ".pmx");
    if (!isEffectModel) return false;
    // The community's controller convention: an "o_" prefix (the MME "o_" shaders) or a name that
    // ends in "controller". A visible accessory the user placed usually does not follow it.
    if (n.rfind("o_", 0) == 0) return true;
    if (ContainsI(n, "controller")) return true;
    return false;
}

PmmEffectSettings MapMmeControllers(const PmmDocument& doc) {
    PmmEffectSettings eff;
    const auto firstKey = [](const PmmAccessory& a) -> const PmmAccessoryKey* {
        return a.keys.empty() ? nullptr : &a.keys.front();
    };
    for (const PmmAccessory& a : doc.accessories) {
        const std::string fn = PathToUtf8(a.path.filename());
        std::string matched;
        const MmeController c = ClassifyMmeController(fn, &matched);
        const PmmAccessoryKey* k = firstKey(a);
        switch (c) {
            case MmeController::AutoLuminous:
                eff.autoLuminous = true;
                eff.recognised.push_back(matched + " -> autoLuminous on");
                break;
            case MmeController::Diffusion:
                eff.bloom = true;
                eff.recognised.push_back(matched + " -> bloom");
                if (k) eff.bloomIntensity = std::clamp(k->scale * 0.5f, 0.0f, 2.0f);
                break;
            case MmeController::Dof:
                eff.dof = true;
                eff.recognised.push_back(matched + " -> depth of field");
                if (k) {
                    eff.dofAperture = std::clamp(k->scale, 0.0f, 3.0f);
                    eff.dofFocusDistance = std::max(1.0f, k->translation.z);
                }
                break;
            case MmeController::Ssao:
                eff.ssao = true;
                eff.recognised.push_back(matched + " -> ambient occlusion");
                break;
            case MmeController::Tone:
                if (k) eff.lutIntensity = std::clamp(k->opacity, 0.0f, 1.0f);
                eff.recognised.push_back(matched + " -> tone / LUT");
                break;
            case MmeController::Shadow:
                eff.recognised.push_back(matched + " -> shadow distance");
                if (k) eff.shadowDistance = std::clamp(k->scale * 160.0f, 5.0f, 1000.0f);
                break;
            case MmeController::LightBloom:
                eff.bloom = true;
                eff.recognised.push_back(matched + " -> bloom");
                break;
            case MmeController::PostEffect:
                eff.recognised.push_back(matched + " -> post effect (no native mapping)");
                break;
            case MmeController::Ray:
                eff.recognised.push_back(matched + " -> Ray-MMD effect (no native mapping)");
                break;
            case MmeController::None:
            default:
                if (LooksLikeUnknownMmeController(a.path)) eff.unsupported.push_back(fn);
                break;
        }
    }
    return eff;
}

} // namespace mmdx
