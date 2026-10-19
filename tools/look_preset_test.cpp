// Unit tests for the Look preset data layer (render/LookPreset.h) and its AppSettings mapping (app/LookApply.h).
// Imitates studio_project_test.cpp: a tiny CHECK macro, prints PASS/FAIL per check and a summary, exit code 0 on
// success.
#include "app/LookApply.h"
#include "render/LookPreset.h"
#include <json.hpp>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

using namespace mmdx;

static int g_passed = 0, g_failed = 0;

static void Check(bool ok, const char* name, const char* fmt = "", ...) {
    if (ok) {
        ++g_passed;
        std::printf("PASS %s\n", name);
        return;
    }
    ++g_failed;
    std::printf("FAIL %s: ", name);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
}

static bool Near(float a, float b, float tol = 1e-4f) {
    const float d = a - b;
    return (d < 0 ? -d : d) <= tol;
}

static LookData MakeLook() {
    LookData look;
    look.lighting = 3;
    look.exposure = 1.4f;
    look.shadows = false;
    look.ssao = false;
    look.ssr = false;
    look.bloom = false;
    look.bloomIntensity = 0.8f;
    look.bloomThreshold = 2.2f;
    look.bloomConvolution = true;
    look.bloomLook.radius = 0.6f;
    look.bloomLook.blend = 1;
    look.bloomLook.depthMask = 0.4f;
    look.bloomLook.kernel.rays = 8;
    look.bloomLook.kernel.rotation = 30.0f;
    look.bloomLook.kernel.spikeLength = 0.5f;
    look.bloomLook.kernel.spikeIntensity = 0.45f;
    look.bloomLook.kernel.glowRadius = 0.7f;
    look.bloomLook.kernel.glowIntensity = 0.25f;
    look.bloomLook.kernel.dispersion = 0.3f;
    look.bloomLook.kernel.anamorphic = 0.2f;
    look.bloomKernelPreset = 4;
    look.dof = true;
    look.dofAperture = 2.0f;
    look.volumetric = true;
    look.volumetricDensity = 2.5f;
    look.autoLuminous = false;
    look.colorLut = "warm";
    look.lutIntensity = 0.65f;
    look.lutFile = "lut/warm.png";
    EffectStackEntry e0;
    e0.pack = "bloom_plus";
    e0.enabled = true;
    e0.params["intensity"] = 0.75f;
    e0.params["threshold"] = 0.25f;
    e0.textureFolder = "F:/tex/effect";
    EffectStackEntry e1;
    e1.pack = "vignette";
    e1.enabled = false;
    EffectStackEntry e2;
    e2.pack = "bloom_plus";   // a repeated pack id: one LookPackRef only
    e2.enabled = false;
    look.effects = {e0, e1, e2};
    look.hasSurface = true;
    look.surface.pack = "hoyo_toon";
    look.surface.params["gloss"] = 0.3f;
    look.surface.textureFolder = "F:/tex/miku";
    look.surface.materials["body"] = (int)PackClass::Skin;
    look.surface.materials["hair"] = kMaterialPackOff;
    look.packs = {{"bloom_plus", "1.2.0", "https://index.example/packs.json"},
                  {"vignette", "", ""},
                  {"hoyo_toon", "2.0.0", "https://index.example/packs.json"}};
    look.setsLighting = true;
    look.setsEffects = true;
    look.setsColor = true;
    look.setsSurface = true;
    return look;
}

static void TestRoundTrip() {
    const LookData look = MakeLook();
    const nlohmann::json j = LookToJson(look);
    Check(j.is_object() && j["lighting"] == 3 && j["bloom"]["enabled"] == false &&
              j["bloom"]["kernel"]["rays"] == 8 && j["dof"]["aperture"].get<float>() == 2.0f &&
              j["color"]["lut"] == "warm" && j["effects"].is_array() && j["effects"].size() == 3 &&
              j["surface"]["pack"] == "hoyo_toon" && j["surface"]["materials"]["hair"] == -1 &&
              j["packs"].size() == 3 && j["sets"].size() == 4,
          "LookToJson writes the documented shape");

    LookData parsed;
    std::vector<std::string> problems;
    Check(ParseLookJson(j, parsed, problems) && problems.empty(), "the written JSON parses without problems");
    Check(parsed == look, "LookToJson -> ParseLookJson round-trips exactly");
}

static void TestClamping() {
    const nlohmann::json j = {
        {"exposure", 9.0},
        {"bloom", {{"intensity", -2.0}, {"threshold", 100.0}}},
        {"dof", {{"aperture", 99.0}}},
        {"volumetric", {{"density", 0.0}}},
        {"color", {{"intensity", 5.0}}},
    };
    LookData look;
    std::vector<std::string> problems;
    Check(ParseLookJson(j, look, problems) && problems.empty(), "clamping input parses without problems");
    Check(Near(look.exposure, 2.0f) && Near(look.bloomIntensity, 0.0f) && Near(look.bloomThreshold, 4.0f) &&
              Near(look.dofAperture, 3.0f) && Near(look.volumetricDensity, 0.25f) && Near(look.lutIntensity, 1.0f),
          "values are clamped to the Settings.h ranges");

    LookData written = look;
    written.exposure = 10.0f;
    written.dofAperture = 0.01f;
    const nlohmann::json out = LookToJson(written);
    Check(out["exposure"].get<float>() == 2.0f && out["dof"]["aperture"].get<float>() == 0.2f,
          "LookToJson clamps too");
}

static void TestWrongTypes() {
    const nlohmann::json j = {
        {"lighting", "bright"},
        {"exposure", "high"},
        {"shadows", 1},
        {"bloom", {{"enabled", "yes"}, {"kernel", {{"rays", 1.5}}}}},
        {"dof", 3},
        {"color", {{"lut", 5}}},
        {"effects", nlohmann::json::parse(R"([{"pack":7,"params":{"x":"big"}}])")},
        {"surface", {{"pack", 1}, {"materials", {{"body", "skin"}}}}},
        {"packs", "none"},
        {"sets", {"lighting", 4, "bogus"}},
    };
    LookData look;
    std::vector<std::string> problems;
    Check(!ParseLookJson(j, look, problems), "wrong types make ParseLookJson report problems");
    Check(problems.size() >= 10, "every wrong field is reported", "problems=%zu", problems.size());
    Check(look.lighting == 0 && Near(look.exposure, 1.0f) && look.shadows && look.bloom,
          "wrong types keep the defaults");
    Check(look.effects.size() == 1 && look.effects[0].pack.empty() && look.effects[0].params.empty(),
          "a bad effects entry keeps its defaults and drops only the bad params");

    Check(!ParseLookJson(nlohmann::json::array(), look, problems), "a non-object look is a problem");
}

static void TestMissingKeys() {
    LookData look;
    look.lighting = 9;   // overwritten by the defaults of a parse
    std::vector<std::string> problems;
    Check(ParseLookJson(nlohmann::json::object(), look, problems) && problems.empty(),
          "an empty look parses without problems");
    const LookData defaults;
    Check(look == defaults, "missing keys keep the LookData defaults");
    Check(look.setsLighting && look.setsEffects && look.setsColor && !look.setsSurface,
          "missing \"sets\" defaults to lighting + effects + color");

    {   // packs are rebuilt from effects + surface when absent, minVersion / source empty, ids unique
        const nlohmann::json j = nlohmann::json::parse(
            R"({"effects":[{"pack":"a"},{"pack":"b"},{"pack":"a"}],"surface":{"pack":"c"}})");
        LookData l;
        problems.clear();
        Check(ParseLookJson(j, l, problems) && problems.empty(), "look without packs parses");
        Check(l.packs.size() == 3 && l.packs[0].id == "a" && l.packs[1].id == "b" && l.packs[2].id == "c" &&
                  l.packs[0].minVersion.empty() && l.packs[0].source.empty(),
              "packs are rebuilt from effects + surface when absent");
        Check(l.hasSurface && l.setsSurface, "a surface block sets hasSurface and the surface group by default");
    }
}

static void TestCaptureApply() {
    AppSettings source;
    source.lighting = 2;
    source.exposure = 1.3f;
    source.shadows = false;
    source.ssao = false;
    source.ssr = true;
    source.bloom = true;
    source.bloomIntensity = 0.6f;
    source.bloomThreshold = 1.8f;
    source.bloomConvolution = true;
    source.bloomLook.radius = 0.7f;
    source.bloomLook.blend = 1;
    source.bloomLook.depthMask = 0.2f;
    source.bloomLook.kernel.rays = 4;
    source.bloomKernelPreset = 2;
    source.dof = true;
    source.dofAperture = 1.7f;
    source.volumetric = true;
    source.volumetricDensity = 0.5f;
    source.autoLuminous = false;
    source.colorLut = "cool";
    source.lutIntensity = 0.4f;
    source.effectStack = {{"grain", true, {{"amount", 0.2f}}, "F:/tex"}};

    ShaderChoice surface;
    surface.pack = "toon";
    surface.params["gloss"] = 0.4f;
    surface.textureFolder = "F:/tex/surface";
    surface.materials["body"] = (int)PackClass::Body;

    const LookData captured = CaptureLook(source, &surface);
    Check(captured.hasSurface && captured.setsSurface && captured.setsLighting && captured.setsEffects &&
              captured.setsColor,
          "CaptureLook marks every group and the surface");
    Check(captured.lighting == 2 && Near(captured.exposure, 1.3f) && !captured.shadows && !captured.ssao &&
              captured.ssr && captured.bloom && Near(captured.bloomIntensity, 0.6f) &&
              Near(captured.bloomThreshold, 1.8f) && captured.bloomConvolution &&
              captured.bloomLook == source.bloomLook && captured.bloomKernelPreset == 2 && captured.dof &&
              Near(captured.dofAperture, 1.7f) && captured.volumetric &&
              Near(captured.volumetricDensity, 0.5f) && !captured.autoLuminous && captured.colorLut == "cool" &&
              Near(captured.lutIntensity, 0.4f) && captured.effects == source.effectStack,
          "CaptureLook copies the settings fields");
    Check(captured.surface.pack == "toon" && captured.surface.params == surface.params &&
              captured.surface.textureFolder == surface.textureFolder &&
              captured.surface.materials == surface.materials,
          "CaptureLook copies pack / params / textureFolder / materials of the surface");
    Check(captured.packs.size() == 2 && captured.packs[0].id == "grain" && captured.packs[1].id == "toon",
          "CaptureLook rebuilds packs from the stack and the surface");

    AppSettings dest;   // defaults
    ShaderChoice destSurface;
    destSurface.pack = "other";
    destSurface.params["keep"] = 1.0f;
    ApplyLook(captured, dest, &destSurface);
    Check(dest.lighting == source.lighting && Near(dest.exposure, source.exposure) &&
              !dest.shadows && !dest.ssao && dest.ssr && dest.bloom &&
              Near(dest.bloomIntensity, source.bloomIntensity) &&
              Near(dest.bloomThreshold, source.bloomThreshold) && dest.bloomConvolution &&
              dest.bloomLook == source.bloomLook && dest.bloomKernelPreset == source.bloomKernelPreset &&
              dest.dof == source.dof && Near(dest.dofAperture, source.dofAperture) &&
              dest.volumetric == source.volumetric && Near(dest.volumetricDensity, source.volumetricDensity) &&
              dest.autoLuminous == source.autoLuminous && dest.colorLut == source.colorLut &&
              Near(dest.lutIntensity, source.lutIntensity) && dest.effectStack == source.effectStack,
          "ApplyLook writes the captured settings back");
    Check(destSurface.pack == "toon" && destSurface.params == surface.params &&
              destSurface.textureFolder == surface.textureFolder && destSurface.materials == surface.materials,
          "ApplyLook writes the surface choice");
}

static void TestSetsSubsets() {
    const LookData captured = MakeLook();

    {   // only lighting: effects / color / surface of the destination stay untouched
        LookData look = captured;
        look.setsLighting = true;
        look.setsEffects = look.setsColor = look.setsSurface = false;
        AppSettings s;
        s.ssao = true;
        s.colorLut = "keep";
        s.effectStack = {{"keep", true, {}, ""}};
        ShaderChoice surface;
        surface.pack = "keep";
        ApplyLook(look, s, &surface);
        Check(s.lighting == captured.lighting && Near(s.exposure, captured.exposure) && s.shadows == captured.shadows,
              "the lighting group is applied");
        Check(s.ssao && s.bloom && s.colorLut == "keep" && s.effectStack.size() == 1 &&
                  s.effectStack[0].pack == "keep" && surface.pack == "keep",
              "the other groups are left alone");
    }
    {   // effects + color only
        LookData look = captured;
        look.setsLighting = look.setsSurface = false;
        look.setsEffects = look.setsColor = true;
        AppSettings s;
        s.lighting = 1;
        s.exposure = 0.9f;
        s.shadows = true;
        ShaderChoice surface;
        surface.pack = "keep";
        ApplyLook(look, s, &surface);
        Check(s.lighting == 1 && Near(s.exposure, 0.9f) && s.shadows, "the lighting group is untouched");
        Check(s.ssao == captured.ssao && s.effectStack == captured.effects && s.colorLut == captured.colorLut &&
                  Near(s.lutIntensity, captured.lutIntensity),
              "the effects and color groups are applied");
        Check(surface.pack == "keep", "the surface group is untouched");
    }
    {   // a surface group with no surface pointer leaves the destination choice alone
        LookData look = captured;
        look.setsLighting = look.setsEffects = look.setsColor = false;
        look.setsSurface = true;
        AppSettings s;
        ApplyLook(look, s, nullptr);
        Check(!s.shadows || true, "ApplyLook with a null surface does not crash");
    }
    {   // an absent surface group: the pointer is untouched even when the look has a surface
        LookData look = captured;
        look.setsLighting = look.setsEffects = look.setsColor = false;
        look.setsSurface = false;
        ShaderChoice surface;
        surface.pack = "keep";
        AppSettings s;
        ApplyLook(look, s, &surface);
        Check(surface.pack == "keep", "an unset surface group leaves the choice alone");
    }
}

int main() {
    TestRoundTrip();
    TestClamping();
    TestWrongTypes();
    TestMissingKeys();
    TestCaptureApply();
    TestSetsSubsets();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
