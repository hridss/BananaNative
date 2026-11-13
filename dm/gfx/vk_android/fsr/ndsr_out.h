// The store of ndsr_easu.comp / ndsr_rcas.comp (vka_draw.c, DM_VK_NDSR): into the game's output image as floats (its B10G11R11 storage view), into the replay's RGBA16F
// intermediate (NDSR_MID), or, where B10G11R11 has no storage use (Adreno with Qualcomm's driver), as the texel's packed 32-bit word through an R32_UINT view of the same
// image (NDSR_PACKED). The colour's end step (ENC): agl's light upscaler (agl_ndsr_vr_light) gets sqrt(colour) as its input
// (its pre-pass a335c1db6270c355 packs it as RGB9E5) and writes the colour, so the passes work on the sqrt values, the space FSR expects (close to gamma 2), and square at the end.
// ENC (specialization constant 0) is that end step: 0 none (the input is the colour), 1 square (sqrt input), 2 sRGB to linear (an upscaler that reads sRGB-encoded colour and decodes
// its output to linear: Splatoon Raiders' agl_ndsr.pck, told by vka_up_color; the passes work on the encoded values, the perceptual space FSR expects)
layout(constant_id = 0) const int ENC = 0;
vec3 ndsr_enc(vec3 c) {
    if (ENC == 1) return c * c;
    if (ENC == 2) { c = clamp(c, 0.0, 1.0); return mix(c * (1.0 / 12.92), pow((c + 0.055) * (1.0 / 1.055), vec3(2.4)), greaterThan(c, vec3(0.04045))); }
    return c;
}
#if defined(NDSR_PACKED)
layout(set = 0, binding = 1, r32ui) uniform writeonly uimage2D dst;
// B10G11R11_UFLOAT from a half's bits: the same exponent bias (15), the mantissa rounded to 6 (5) bits; a carry into the exponent is the right rounding; clamped below infinity
uint ndsr_uf(float v, uint drop, uint maxv) {
    uint h = packHalf2x16(vec2(clamp(v, 0.0, 65000.0), 0.0)) & 0x7fffu;   // (clamp: negatives and NaN to 0, past the formats' largest to it)
    return min((h + (1u << (drop - 1u))) >> drop, maxv);
}
void ndsr_store(ivec2 p, vec3 c) {
    c = ndsr_enc(c);
    imageStore(dst, p, uvec4(ndsr_uf(c.r, 4u, 0x7bfu) | ndsr_uf(c.g, 4u, 0x7bfu) << 11 | ndsr_uf(c.b, 5u, 0x3dfu) << 22, 0u, 0u, 0u));
}
#elif defined(NDSR_MID)
layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D dst;
void ndsr_store(ivec2 p, vec3 c) { c = ndsr_enc(c); imageStore(dst, p, vec4(c, 1.0)); }   // (RGBA16F: signed, kept as it is for RCAS)
#else
layout(set = 0, binding = 1, r11f_g11f_b10f) uniform writeonly image2D dst;
// As the packed store: no negative (an unsigned float format's conversion of one is not defined) and no NaN (a NaN texel spreads through every later blur and average)
void ndsr_store(ivec2 p, vec3 c) { c = ndsr_enc(c); c = mix(max(c, vec3(0.0)), vec3(0.0), isnan(c)); imageStore(dst, p, vec4(c, 1.0)); }
#endif
