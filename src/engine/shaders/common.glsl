// Shared colour maths. Must match frame.h to the digit.
vec4 alphaTap(sampler2D tex, ivec2 p) {
    vec4 c = texelFetch(tex, clamp(p, ivec2(0), textureSize(tex, 0) - 1), 0);
    return vec4(c.rgb * c.a, c.a);
}
vec4 alphaBilinear(sampler2D tex, vec2 uv) {
    vec2 p = uv * vec2(textureSize(tex, 0)) - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = p - vec2(i);
    vec4 a = alphaTap(tex, i), b = alphaTap(tex, i + ivec2(1, 0));
    vec4 c = alphaTap(tex, i + ivec2(0, 1)), d = alphaTap(tex, i + ivec2(1, 1));
    vec4 top = a + (b - a) * f.x, bottom = c + (d - c) * f.x;
    return top + (bottom - top) * f.y;
}
// Associated colour back to straight. Colour below zero is kept: working
// light may be negative (out of gamut, below black), as the colour-managed
// pipeline's working frames are.
vec4 straightSample(vec4 c) {
    if (c.a <= 0.0) return vec4(0.0);
    return vec4(c.rgb / c.a, clamp(c.a, 0.0, 1.0));
}
float srgbToLinear(float c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
vec3 srgbToLinear(vec3 c) {
    return vec3(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b));
}
float linearToSrgb(float c) {
    if (c <= 0.0) return 0.0;
    if (c >= 1.0) return 1.0;
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
vec3 linearToSrgb(vec3 c) {
    return vec3(linearToSrgb(c.r), linearToSrgb(c.g), linearToSrgb(c.b));
}
// The Rec. 709 curve, as the timeline's codes have it (colour/oma-video-v1).
float rec709Encode(float c) {
    if (c <= 0.0) return 0.0;
    if (c >= 1.0) return 1.0;
    return c < 0.018 ? c * 4.5 : 1.099 * pow(c, 0.45) - 0.099;
}
vec3 rec709Encode(vec3 c) {
    return vec3(rec709Encode(c.r), rec709Encode(c.g), rec709Encode(c.b));
}
float rec709Decode(float c) {
    return c < 0.081 ? c / 4.5 : pow((c + 0.099) / 1.099, 1.0 / 0.45);
}
vec3 rec709Decode(vec3 c) {
    return vec3(rec709Decode(c.r), rec709Decode(c.g), rec709Decode(c.b));
}
uint to8(float v) {
    return uint(clamp(int(round(v)), 0, 255));
}

// Resampling weights, word for word as frame.h.
float cubicWeight(float x) {
    x = abs(x);
    if (x < 1.0) return 1.5 * x * x * x - 2.5 * x * x + 1.0;
    if (x < 2.0) return -0.5 * x * x * x + 2.5 * x * x - 4.0 * x + 2.0;
    return 0.0;
}
float lanczosWeight(float x) {
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 3.0) return 0.0;
    float px = 3.14159265358979 * x;
    return 3.0 * sin(px) * sin(px / 3.0) / (px * px);
}
int areaTaps(float footprint) {
    return clamp(int(ceil(footprint - 0.001)), 1, 8);
}
float lumaOf(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

float blendChannel(int mode, float d, float s) {
    if (mode == 1) return d * s;
    if (mode == 2) return 1.0 - (1.0 - d) * (1.0 - s);
    if (mode == 3) return d < 0.5 ? 2.0 * d * s : 1.0 - 2.0 * (1.0 - d) * (1.0 - s);
    if (mode == 4) return d < s ? d : s;
    if (mode == 5) return d > s ? d : s;
    if (mode == 6) return min(d + s, 1.0);
    if (mode == 7) return max(d - s, 0.0);
    if (mode == 8) return abs(d - s);
    if (mode == 9) return d <= 0.0 ? 0.0 : s >= 1.0 ? 1.0 : min(1.0, d / (1.0 - s));
    if (mode == 10) return d >= 1.0 ? 1.0 : s <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - d) / s);
    if (mode == 11) return s < 0.5 ? 2.0 * d * s : 1.0 - 2.0 * (1.0 - d) * (1.0 - s);
    if (mode == 12) {
        if (s <= 0.5) return d - (1.0 - 2.0 * s) * d * (1.0 - d);
        float e = d <= 0.25 ? ((16.0 * d - 12.0) * d + 4.0) * d : sqrt(d);
        return d + (2.0 * s - 1.0) * (e - d);
    }
    if (mode == 13) return d + s - 2.0 * d * s;
    return s;
}

// BT.2100's HDR transfers, word for word as frame.h.
float pqEncode(float y) {
    y = clamp(y, 0.0, 1.0);
    float p = pow(y, 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375);
}
float pqDecode(float e) {
    e = clamp(e, 0.0, 1.0);
    float p = pow(e, 1.0 / 78.84375), n = p - 0.8359375;
    return pow(max(n, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}
float hlgEncode(float e) {
    e = clamp(e, 0.0, 1.0);
    return e <= 1.0 / 12.0 ? sqrt(3.0 * e) : 0.17883277 * log(12.0 * e - 0.28466892) + 0.55991073;
}
float hlgDecode(float v) {
    v = clamp(v, 0.0, 1.0);
    return v <= 0.5 ? v * v / 3.0 : (exp((v - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
}
