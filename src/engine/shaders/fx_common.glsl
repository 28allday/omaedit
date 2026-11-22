// What every effect shader shares: its bindings, its push block (FilterPush
// in vk_engine.cpp: the step's input and output sizes, the effect's kind,
// the pass in `mode`, the clip's frame in `ix`, its values in p), and the
// helpers its CPU twin repeats word for word (fx.h).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(local_size_x = 16, local_size_y = 16) in;
layout(binding = 0) uniform sampler2D src;   // this pass's input
layout(binding = 1, rgba32f) uniform writeonly image2D dst;
layout(binding = 2) uniform sampler2D aux;   // the step's input
layout(push_constant) uniform Push {
    ivec2 size;
    ivec2 outSize;
    int kind, pass, frame, extra;
    vec4 p[6];
} pc;

float P(int i) { return pc.p[i >> 2][i & 3]; }
// The picture drawn against the one it stands for (VideoFilter::scale).
float unit() { return pc.p[5][3] > 0.0 ? pc.p[5][3] : 1.0; }
vec4 at(ivec2 q) { return texelFetch(src, clamp(q, ivec2(0), pc.size - 1), 0); }
vec4 assoc(vec4 c) { return vec4(c.rgb * c.a, c.a); }
// A straight-alpha sample at a pixel position (centres at .5), bilinear on
// associated colour, transparent outside the picture.
vec4 sampleAt(vec2 q) {
    vec2 f = q - 0.5;
    ivec2 i = ivec2(floor(f));
    vec2 t = f - vec2(i);
    vec4 s = vec4(0.0);
    for (int j = 0; j < 2; ++j)
        for (int k = 0; k < 2; ++k) {
            ivec2 c = i + ivec2(k, j);
            vec4 v = any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, pc.size)) ? vec4(0.0) : assoc(texelFetch(src, c, 0));
            s += v * (k == 1 ? t.x : 1.0 - t.x) * (j == 1 ? t.y : 1.0 - t.y);
        }
    return straightSample(s);
}
// sampleAt for the step's input (aux) rather than this pass's.
vec4 sampleAux(vec2 q) {
    vec2 f = q - 0.5;
    ivec2 i = ivec2(floor(f));
    vec2 t = f - vec2(i);
    vec4 s = vec4(0.0);
    for (int j = 0; j < 2; ++j)
        for (int k = 0; k < 2; ++k) {
            ivec2 c = i + ivec2(k, j);
            vec4 v = any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, pc.size)) ? vec4(0.0) : assoc(texelFetch(aux, c, 0));
            s += v * (k == 1 ? t.x : 1.0 - t.x) * (j == 1 ? t.y : 1.0 - t.y);
        }
    return straightSample(s);
}
// One direction of a Gaussian of `sigma` pixels about q, reaching three
// sigma, on associated colour: at most 33 taps a side, spread out beyond
// that. Returns associated colour, edges held.
vec4 gaussLine(vec2 q, vec2 dir, float sigma) {
    if (sigma < 0.25) return assoc(sampleAt(clamp(q, vec2(0.5), vec2(pc.size) - 0.5)));
    int n = min(32, int(ceil(3.0 * sigma)));
    float step_ = 3.0 * sigma / float(n);
    vec4 s = vec4(0.0);
    float w = 0.0;
    for (int i = -n; i <= n; ++i) {
        float x = float(i) * step_, k = exp(-0.5 * (x / sigma) * (x / sigma));
        s += k * assoc(sampleAt(clamp(q + dir * x, vec2(0.5), vec2(pc.size) - 0.5)));
        w += k;
    }
    return s / w;
}

// Noise: a hash of integers to 0..1, the same as fxHash in fx.h.
float fxHash(int x, int y, int seed) {
    uint h = uint(x) * 0x8da6b343u ^ uint(y) * 0xd8163841u ^ uint(seed) * 0xcb1ab31fu;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return float(h >> 8) * (1.0 / 16777216.0);
}
vec3 display(vec3 c) { return clamp(linearToSrgb(c), 0.0, 1.0); }
vec3 light(vec3 e) { return srgbToLinear(clamp(e, 0.0, 1.0)); }

vec4 effect(ivec2 p);
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(p, pc.outSize))) return;
    imageStore(dst, p, effect(p));
}
