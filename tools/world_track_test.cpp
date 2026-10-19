// Unit test for WorldColliderTrack::Sample (render/WorldTypes.cpp): interpolation, clamping at both ends and the
// velocities carried from the samples. A tiny CHECK macro like look_preset_test.cpp.
#include "render/WorldTypes.h"
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

static bool Near(double a, double b, double tol = 1e-5) {
    const double d = a - b;
    return (d < 0 ? -d : d) <= tol;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // 3 samples at 2 Hz (0.5 s apart), one collider each: positions 0, 1, 2 along x, velocities -1, 0.5, 4.
    WorldColliderTrack track;
    track.rate = 2.0f;
    track.start = 1.0;
    track.perSample = 1;
    track.sampleCount = 3;
    track.samples = {
        {{0.0f, 0.0f, 0.0f}, 0.6f, {-1.0f, 0.0f, 0.0f}, kColliderFoot | kColliderLeft},
        {{1.0f, 0.0f, 0.0f}, 0.6f, {0.5f, 0.0f, 0.0f}, kColliderFoot | kColliderLeft},
        {{2.0f, 0.0f, 0.0f}, 0.4f, {4.0f, 0.0f, 0.0f}, kColliderHand},
    };
    std::vector<WorldCollider> out;

    track.Sample(1.0, out);
    Check(out.size() == 1 && Near(out[0].pos.x, 0.0), "sample at the first time is the first sample");

    track.Sample(1.25, out);
    Check(out.size() == 1 && Near(out[0].pos.x, 0.5) && Near(out[0].vel.x, -0.25),
          "between samples: positions and velocities interpolate linearly");

    track.Sample(1.5, out);
    Check(Near(out[0].pos.x, 1.0) && Near(out[0].vel.x, 0.5), "sample landing on a key returns it exactly");

    track.Sample(1.6, out);
    Check(Near(out[0].pos.x, 1.2) && Near(out[0].vel.x, 1.2) && out[0].radius == 0.6f,
          "second interval interpolates (radius from the nearer sample)");

    track.Sample(1.9, out);
    Check(Near(out[0].pos.x, 1.8) && out[0].radius == 0.4f && out[0].flags == kColliderHand,
          "radius / flags come from the nearer sample");

    track.Sample(-5.0, out);
    Check(Near(out[0].pos.x, 0.0) && Near(out[0].vel.x, -1.0), "before the start clamps to the first sample");

    track.Sample(50.0, out);
    Check(Near(out[0].pos.x, 2.0) && Near(out[0].vel.x, 4.0), "after the end clamps to the last sample");

    // out is resized to perSample; an empty track clears it.
    track.perSample = 2;
    track.sampleCount = 1;
    track.samples = {
        {{0.0f, 1.0f, 2.0f}, 0.6f, {1.0f, 2.0f, 3.0f}, kColliderFoot},
        {{3.0f, 4.0f, 5.0f}, 0.4f, {-1.0f, -2.0f, -3.0f}, kColliderHand},
    };
    out.resize(9);
    track.Sample(10.0, out);
    Check(out.size() == 2 && Near(out[0].pos.y, 1.0) && Near(out[1].pos.z, 5.0) && Near(out[1].vel.z, -3.0),
          "one sample: out is resized to perSample and holds both colliders");

    WorldColliderTrack empty;
    out.resize(4);
    empty.Sample(1.0, out);
    Check(out.empty(), "a track without colliders clears out");

    std::printf("world_track_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
