// FFT convolution bloom (render/BloomFft.cpp): the thresholded HDR image (rgb + a = luminance-weighted linear
// depth) is convolved with a procedural kernel per channel. Every channel is its own complex line, two per texel,
// so the grid runs twice: slot 0 = (R, G), slot 1 = (B, A). Row/column FFTs are radix-2 Stockham passes in
// groupshared memory; the columns are multiplied by the kernel spectrum of the same slot (t1, each channel
// normalised by its own DC term = kernel sum) and the inverse transform writes the convolved image.
#ifndef FFT_N
#define FFT_N 512
#endif
#ifndef FFT_LOG2
#define FFT_LOG2 9
#endif
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gLinear : register(s1);
Texture2D<float4> gIn : register(t0);
Texture2D<float4> gKernelSpec : register(t1);   // kernel spectrum (CSFftCols) / slot 1 grid (CSFftOutput)
RWTexture2D<float4> gOut : register(u0);
static const float kPi = 3.14159265;
groupshared float4 gs[2][FFT_N];
float2 CMul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

// FFT of one line held in gs[src], executed by FFT_N/2 threads (thread index j).
// Runs log2(N) passes; returns the buffer index holding the result. dirSign = -1 forward, +1 inverse.
uint FftLine(uint j, uint src, float dirSign) {
    for (uint Ns = 1; Ns < FFT_N; Ns <<= 1) {
        float4 v0 = gs[src][j];
        float4 v1 = gs[src][j + FFT_N / 2];
        uint k = j & (Ns - 1);
        float a = dirSign * 2.0 * kPi * (float)k / (float)(Ns * 2);
        float2 w = float2(cos(a), sin(a));
        v1 = float4(CMul(v1.xy, w), CMul(v1.zw, w));
        uint d = (j / Ns) * Ns * 2 + k;
        gs[src ^ 1][d] = v0 + v1;
        gs[src ^ 1][d + Ns] = v0 - v1;
        GroupMemoryBarrierWithGroupSync();
        src ^= 1;
    }
    return src;
}

// Forward or inverse FFT of row gid.y. gP0.x > 0.5 selects inverse (results scaled by 1/N).
[numthreads(FFT_N / 2, 1, 1)]
void CSFftRows(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint j = tid.x;
    uint ln = gid.y;
    gs[0][j] = gIn[int2(j, ln)];
    gs[0][j + FFT_N / 2] = gIn[int2(j + FFT_N / 2, ln)];
    GroupMemoryBarrierWithGroupSync();
    uint res = FftLine(j, 0, gP0.x > 0.5 ? 1.0 : -1.0);
    float4 a = gs[res][j], b = gs[res][j + FFT_N / 2];
    if (gP0.x > 0.5) {
        a /= (float)FFT_N;
        b /= (float)FFT_N;
    }
    gOut[int2(j, ln)] = a;
    gOut[int2(j + FFT_N / 2, ln)] = b;
}

// Forward FFT of column gid.y; in convolve mode (gP0.x > 0.5) the spectrum is multiplied by the
// kernel spectrum (t1, same slot layout) and the line is transformed back (scaled by 1/N).
[numthreads(FFT_N / 2, 1, 1)]
void CSFftCols(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint j = tid.x;
    uint ln = gid.y;
    gs[0][j] = gIn[int2(ln, j)];
    gs[0][j + FFT_N / 2] = gIn[int2(ln, j + FFT_N / 2)];
    GroupMemoryBarrierWithGroupSync();
    uint res = FftLine(j, 0, -1.0);
    if (gP0.x > 0.5) {
        // the kernel is real, so both complex pairs of a texel use the same K
        float4 v0 = gs[res][j];
        float4 v1 = gs[res][j + FFT_N / 2];
        // one real kernel per complex slot, each normalised by its own sum
        float2 dc = 1.0 / max(gKernelSpec[int2(0, 0)].xz, 1e-20);
        float4 k0 = gKernelSpec[int2(ln, j)] * dc.xxyy;
        float4 k1 = gKernelSpec[int2(ln, j + FFT_N / 2)] * dc.xxyy;
        gs[res][j] = float4(CMul(v0.xy, k0.xy), CMul(v0.zw, k0.zw));
        gs[res][j + FFT_N / 2] = float4(CMul(v1.xy, k1.xy), CMul(v1.zw, k1.zw));
        GroupMemoryBarrierWithGroupSync();
        res = FftLine(j, res, 1.0);
        gs[res][j] /= (float)FFT_N;
        gs[res][j + FFT_N / 2] /= (float)FFT_N;
    }
    gOut[int2(ln, j)] = gs[res][j];
    gOut[int2(ln, j + FFT_N / 2)] = gs[res][j + FFT_N / 2];
}

// Thresholded image into the grid as two real channels of one slot. gP0 = content offset xy, content size zw (grid
// pixels); gP1.x = slot (0: R, G; 1: B, A).
[numthreads(8, 8, 1)]
void CSInput(uint3 id : SV_DispatchThreadID) {
    float2 uv = (float2(id.xy) + 0.5 - gP0.xy) / gP0.zw;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        gOut[id.xy] = 0;
        return;
    }
    float4 c = max(gIn.SampleLevel(gLinear, uv, 0), 0.0);
    float2 v = gP1.x > 0.5 ? c.ba : c.rg;
    gOut[id.xy] = float4(v.x, 0.0, v.y, 0.0);
}

// ---- kernel -------------------------------------------------------------------------------------------------------
// One channel of the kernel at grid offset d (pixels; the content is cw grid pixels wide, so lengths are fractions
// of the screen width times cw). Three parts, each with unit integral, weighted by its energy share:
//   core  - a pixel-sized Gaussian (the light that does not scatter),
//   glow  - a Moffat profile (1 + r^2/r0^2)^-beta: flat near the source, power-law tail (scatter in the lens / eye),
//   rays  - diffraction spikes: thin lines through the centre, falling off as a power of the distance, faded out
//           at their maximum length, slightly widening outward.
// gP0 = (cw, rays, rotation rad, slot), gP1 = (spikeLength, spikeIntensity, glowRadius, glowIntensity),
// gP2 = (dispersion, anamorphic). The channel scale stretches the whole kernel (dispersion: red reaches farther).
static const float kGlowBeta = 1.35;
static const float kCoreSigma = 0.7;

float SpikeProfile(float x, float l0, float lmax) {
    return pow(1.0 + x / l0, -1.7) * saturate(1.0 - x / lmax) * saturate(1.0 - x / lmax);
}
float SpikeWidth(float x) { return 0.55 + x * 0.006; }

float KernelChannel(float2 d, float scale) {
    const float cw = gP0.x;
    const int lines = (int)(gP0.y * 0.5 + 0.25);
    float2 p = d / scale;
    float r2 = dot(p, p);

    float spikeShare = lines > 0 ? 0.45 * gP1.y : 0.0;
    float glowShare = 0.9 * gP1.w * (1.0 - spikeShare);
    float coreShare = max(1.0 - spikeShare - glowShare, 0.0);

    float k = coreShare * exp(-r2 / (2.0 * kCoreSigma * kCoreSigma)) / (2.0 * kPi * kCoreSigma * kCoreSigma);

    // glow (anamorphic: wider horizontally)
    float stretch = 1.0 + 5.0 * gP2.y;
    float r0 = cw * lerp(0.002, 0.06, gP1.z * gP1.z) + 0.5;
    float2 pg = float2(p.x / stretch, p.y);
    float moffat = (kGlowBeta - 1.0) / (kPi * r0 * r0 * stretch) * pow(1.0 + dot(pg, pg) / (r0 * r0), -kGlowBeta);
    k += glowShare * moffat;

    if (lines > 0 && spikeShare > 0.0) {
        float l0 = cw * lerp(0.0015, 0.02, gP1.x) + 0.5;
        float lmax = cw * lerp(0.05, 0.9, gP1.x);
        // the 1D integral of one ray (perpendicular Gaussian integrates to sqrt(2 pi) w(x))
        float integral = 0.0;
        for (float x = 0.5; x < FFT_N * 0.5; x += 1.0) integral += SpikeProfile(x, l0, lmax) * SpikeWidth(x);
        integral *= 2.0 * sqrt(2.0 * kPi);
        float rays = 0.0;
        for (int s = 0; s < lines; ++s) {
            float a = gP0.z + s * kPi / lines;
            float2 axis = float2(cos(a), sin(a));
            float along = abs(dot(p, axis));
            float perp = abs(dot(p, float2(-axis.y, axis.x)));
            float w = SpikeWidth(along);
            rays += SpikeProfile(along, l0, lmax) * exp(-perp * perp / (2.0 * w * w));
        }
        k += spikeShare * rays / (integral * lines);
    }
    return k;
}

// Kernel grid of one slot, centred at grid pixel (0,0) with wrap-around: slot 0 = (R, G), slot 1 = (B, G) (the
// depth channel A is spread with the green kernel). Each channel as a real complex number.
[numthreads(8, 8, 1)]
void CSKernel(uint3 id : SV_DispatchThreadID) {
    float2 d = float2((int2(id.xy) + FFT_N / 2) % FFT_N) - FFT_N / 2;
    float window = saturate((FFT_N * 0.5 - length(d)) / 32.0);   // no wrap-around at the grid edge
    float disp = gP2.x;
    float first = gP0.w > 0.5 ? 1.0 - 0.18 * disp : 1.0 + 0.25 * disp;   // B or R
    gOut[id.xy] = float4(KernelChannel(d, first) * window, 0.0, KernelChannel(d, 1.0) * window, 0.0);
}

// Convolved grids -> the bloom image: t0 = slot 0 (R, G), t1 = slot 1 (B, A) (bound where the kernel spectrum is). gP0 = content offset / N (xy), content
// size / N (zw); gP1.x = gain, gP1.yz = target size in pixels.
[numthreads(8, 8, 1)]
void CSFftOutput(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)gP1.y || id.y >= (uint)gP1.z) return;
    float2 uv = gP0.xy + (float2(id.xy) + 0.5) / gP1.yz * gP0.zw;
    float4 a = gIn.SampleLevel(gLinear, uv, 0), b = gKernelSpec.SampleLevel(gLinear, uv, 0);
    gOut[id.xy] = max(float4(a.x, a.z, b.x, b.z), 0.0) * gP1.x;
}
